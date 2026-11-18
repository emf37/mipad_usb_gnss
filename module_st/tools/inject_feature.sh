#!/system/bin/sh
# 特性/VINTF 注入：把 GPS 特性声明与 GNSS HAL 的 VINTF fragment 以 bind-mount
# 叠加到 init 挂载命名空间的 /vendor 对应目录，配合软重启让全新 system_server
# 在开机扫描时读到（半持久越狱下 magic mount 不可用时的替代通道）。
#
# 用法：su -c sh /data/adb/gnss_usb/inject_feature.sh [status|apply|clean]
STAGE=/data/adb/gnss_usb/stage
GPSSRC="$(dirname "$0")"
cmd="${1:-apply}"

# nsenter 可用则进 init 命名空间（确保软重启后的 system_server 可见）；否则
# 退化为当前 shell 直接挂载（KSU 类 root 通常与 init 同命名空间，同样生效）。
nsmount() {
    if command -v nsenter >/dev/null 2>&1; then
        nsenter -t 1 -m -- "$@"
    else
        "$@"
    fi
}

do_status() {
    echo "--- /vendor/etc/permissions ---"
    timeout 5 ls /vendor/etc/permissions/ 2>&1 | head -20
    echo "--- gps feature file present? ---"
    timeout 5 ls -Z /vendor/etc/permissions/ 2>/dev/null | grep -i "location.gps" || echo "not present"
    echo "--- bind mounts ---"
    timeout 5 mount 2>/dev/null | grep -E "gnss_usb|stage" || echo "no overlay active"
    echo "--- pm feature (当前PMS缓存) ---"
    timeout 10 pm has-feature android.hardware.location.gps && echo "feature: YES" || echo "feature: NO (软重启后生效)"
}

do_clean() {
    nsmount umount /vendor/etc/vintf/manifest 2>/dev/null
    nsmount umount /vendor/etc/permissions 2>/dev/null
    rm -rf "$STAGE"
    echo "cleaned"
}

do_apply() {
    # 0) 【关键顺序】先卸载旧 overlay，让 /vendor/etc/permissions 回到真实目录。
    #    旧实现先 rm -rf $STAGE——而 stage 正是已挂载 overlay 的源，等于先把自己的
    #    内容删空、再从空目录拷贝，第二次起 vendor 特性清单被洗掉
    #    （曾导致 FEATURE_WIFI/FEATURE_FINGERPRINT 消失→system_server崩溃）。
    nsmount umount /vendor/etc/vintf/manifest 2>/dev/null
    nsmount umount /vendor/etc/permissions 2>/dev/null
    sleep 1

    # 1) staging：现在拷贝的一定是真实目录
    rm -rf "$STAGE"
    mkdir -p "$STAGE/permissions" "$STAGE/vintf"

    if ! cp -a /vendor/etc/permissions/. "$STAGE/permissions/" 2>/dev/null; then
        echo "[!] cannot copy /vendor/etc/permissions"; return 1
    fi

    # 特性自愈：实测 cp -a 拷贝 /vendor/etc/permissions 会丢文件（丢过
    # android.hardware.wifi.xml → FEATURE_WIFI=false → WifiP2pService 构造
    # 崩 → system_server 连环崩）。因此无条件（重）写以下声明，重复声明无害：
    write_feature() {
        cat > "$STAGE/permissions/$1.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<permissions>
    <feature name="$1" />
</permissions>
EOF
    }
    write_feature android.hardware.location.gps
    write_feature android.hardware.wifi
    write_feature android.hardware.wifi.direct

    # 核对拷贝完整性：缺任何一个源文件都绝不挂载（防止特性清单被洗掉的
    # 悲剧重演——那会删掉指纹/wifi等特性甚至导致system_server崩溃）
    SRC_COUNT=$(ls /vendor/etc/permissions 2>/dev/null | wc -l)
    MISSING=0
    for f in /vendor/etc/permissions/*; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        if [ ! -e "$STAGE/permissions/$b" ]; then
            echo "[!] staged missing: $b"
            MISSING=$((MISSING+1))
        fi
    done
    DST_COUNT=$(ls "$STAGE/permissions" | wc -l)
    echo "[i] permissions: source=$SRC_COUNT staged=$DST_COUNT missing=$MISSING"
    if [ "$MISSING" != "0" ] || [ "$DST_COUNT" -lt "$SRC_COUNT" ]; then
        echo "[ABORT] staging 不完整，拒绝挂载（当前 overlay 已卸载，系统用真实目录，安全）"
        return 1
    fi

    if [ -d /vendor/etc/vintf/manifest ]; then
        cp -a /vendor/etc/vintf/manifest/. "$STAGE/vintf/" 2>/dev/null
        # 格式与真机 stock fragment 完全同构（manifest meta-version=8.0，
        # 参照 android.hardware.wifi-service.xml：version+fqname 写法）
        cat > "$STAGE/vintf/android.hardware.gnss-usb.xml" <<'EOF'
<manifest version="8.0" type="device">
    <hal format="aidl">
        <name>android.hardware.gnss</name>
        <version>4</version>
        <fqname>IGnss/default</fqname>
    </hal>
</manifest>
EOF
        # vintf 完整性校验（与 permissions 同标准：缺文件绝不挂载）
        VMISS=0
        for f in /vendor/etc/vintf/manifest/*; do
            [ -e "$f" ] || continue
            [ -e "$STAGE/vintf/$(basename "$f")" ] || { echo "[!] vintf staged missing: $(basename "$f")"; VMISS=$((VMISS+1)); }
        done
        if [ "$VMISS" != "0" ]; then
            echo "[ABORT] vintf staging 不完整，拒绝挂载"
            return 1
        fi
    else
        echo "[i] /vendor/etc/vintf/manifest 不存在，跳过VINTF叠加"
    fi

    # 2) SELinux 标签对齐 vendor 配置文件（system_server 才能读）
    chcon -R u:object_r:vendor_configs_file:s0 "$STAGE" 2>/dev/null \
        || echo "[!] chcon 失败（部分工具不支持），继续尝试"
    chmod 644 "$STAGE"/permissions/* "$STAGE"/vintf/* 2>/dev/null

    # 3) bind-mount 叠加（进 init 命名空间，软重启后依然有效；旧叠加已在步骤0卸载）
    if ! nsmount mount --bind "$STAGE/permissions" /vendor/etc/permissions; then
        echo "[!] permissions 叠加失败"; return 1
    fi
    [ -d "$STAGE/vintf" ] && [ -d /vendor/etc/vintf/manifest ] && \
        nsmount mount --bind "$STAGE/vintf" /vendor/etc/vintf/manifest \
        || echo "[i] vintf 叠加未做"

    echo "[OK] 叠加完成："
    timeout 5 ls /vendor/etc/permissions/ | grep -i "location.gps" \
        && echo "    GPS feature xml 可见"
    [ -f /vendor/etc/vintf/manifest/android.hardware.gnss-usb.xml ] \
        && echo "    VINTF fragment 可见"
    echo "下次软重启（或点模块执行操作）后 system_server 将声明 GPS 支持"
}

case "$cmd" in
    status) do_status ;;
    clean)  do_clean ;;
    apply)  do_apply ;;
    *)      echo "usage: $0 [status|apply|clean]" ;;
esac
