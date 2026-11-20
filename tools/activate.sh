#!/system/bin/sh
# 激活 GNSS HAL（半持久模式）。
# 用法：激活root后
#   su -c sh /data/adb/gnss_usb/activate.sh           # 启动守护 + 体检
#   su -c sh /data/adb/gnss_usb/activate.sh soft      # 启动守护 + 软重启(推荐)
#   su -c sh /data/adb/gnss_usb/activate.sh stop      # 停止
#   su -c sh /data/adb/gnss_usb/activate.sh status    # 只查状态
#
# soft 说明：只杀 zygote（servicemanager 与已注册的 HAL 服务保留），
# system_server 全新启动时即能看到 IGnss/default，等同"开机即有GNSS"。
# 若你的越狱工具自带的软重启是 stop;start 式（servicemanager 会重启），
# 守护进程内置的看门狗会在 2 秒内自动补注册，同样兼容。
DEST=/data/adb/gnss_usb
BIN="$DEST/bin/gnss_usb_hal"
LOG="$DEST/hal.log"
PIDFILE="$DEST/supervisor.pid"

cmd="${1:-start}"

# 任意子命令执行时都先清 RescueParty（崩溃计数遗留会在重启时升级自救级别）
if [ -f "$DEST/clear_rescue.sh" ]; then
    sh "$DEST/clear_rescue.sh" >/dev/null 2>&1
fi

stop_all() {
    [ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null
    pkill -f gnss_usb_hal 2>/dev/null
    rm -f "$PIDFILE"
    echo "stopped"
}

start_daemon() {
    # 守护存活保护：框架持有 binder 时杀守护会触发 HyperOS android.fg 死锁
    # → Watchdog 预杀死机（dropbox 实证 system_server_pre_watchdog）。
    # 已注册且健康 → 不重启，直接返回。
    if timeout 10 service list 2>/dev/null | grep -q "android.hardware.gnss.IGnss"; then
        echo "[i] 守护进程已在运行且服务已注册——跳过重启（避免fg死锁）"
        return 0
    fi
    stop_all
    pkill -f gnss_usb_hal 2>/dev/null
    sleep 1
    setsid sh -c '
      echo $$ > "'"$PIDFILE"'"
      cd "'"$DEST"'"
      while true; do
        [ "$(cat "'"$PIDFILE"'" 2>/dev/null)" = "$$" ] || exit 0
        "'"$BIN"'" >> "'"$LOG"'" 2>&1
        sleep 3
      done
    ' >/dev/null 2>&1 &
    sleep 2
    if service list 2>/dev/null | grep -q "android.hardware.gnss.IGnss"; then
        service list | grep gnss
        echo "[OK] HAL 服务已注册"
        return 0
    fi
    echo "[FAIL] 服务未注册，hal.log 尾部："
    tail -5 "$LOG" 2>/dev/null
    return 1
}

# 刷新 servicemanager 的 VINTF 缓存：
# libvintf 以 /apex/apex-info.xml 的 mtime 为缓存失效键，但该文件受 SELinux
# 保护连 root 都不能 touch。改用 bind-mount：staging 一个带新 mtime 的副本
# 盖上去（挂进 init 命名空间）；之后刷新只需 touch staging 源文件。
refresh_vintf_cache() {
    # 失效键真身（真机libvintf字符串证实）：/apex/apex-info-list.xml
    local SRC=/apex/apex-info-list.xml
    local STG="$DEST/stage-apex-info-list.xml"
    # 清理早前盖错文件的挂载（/apex/apex-info.xml）
    nsenter -t 1 -m -- umount /apex/apex-info.xml 2>/dev/null
    mkdir -p "$DEST"
    cp "$SRC" "$STG" 2>/dev/null || : > "$STG"   # 保留真实内容（apex清单）
    touch "$STG"                                   # 关键：mtime=now，触发libvintf重读
    chmod 644 "$STG"
    LBL=$(ls -Z /apex 2>/dev/null | awk 'NR==2{print $1}')
    [ -n "$LBL" ] && chcon "$LBL" "$STG" 2>/dev/null
    if timeout 5 mount 2>/dev/null | grep -q " on $SRC "; then
        touch "$STG"
        echo "[OK] VINTF缓存已刷新（apex-info-list 新mtime）"
    elif nsenter -t 1 -m -- mount --bind "$STG" "$SRC" 2>/dev/null \
         || mount --bind "$STG" "$SRC" 2>/dev/null; then
        echo "[OK] VINTF失效键已接管（bind-mount apex-info-list.xml，新mtime）"
    else
        echo "[!] apex-info-list 覆盖失败，VINTF缓存无法刷新"
        return 1
    fi
}

soft_reboot() {
    # 会话级软重启保护：实测同一开机会话内第二次 soft 会死机（机理未明，
    # 冷启动后首次 soft 安全）。日常流程（开机→激活root→soft）每会话只有
    # 一次，不受影响。开发调试需强制：setprop persist.vendor.gnss_usb.soft_force 1
    local BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
    local MARK="$DEST/soft_boot_id"
    if [ "$(getprop persist.vendor.gnss_usb.soft_force)" != "1" ] && \
       [ -f "$MARK" ] && [ "$(cat "$MARK" 2>/dev/null)" = "$BOOT_ID" ]; then
        echo "[!] 本会话已执行过软重启——重复 soft 实测会导致死机，已拒绝。"
        echo "    如需再跑：整机重启后激活root再来，或临时强制："
        echo "    setprop persist.vendor.gnss_usb.soft_force 1 && sh $DEST/activate.sh soft"
        exit 1
    fi
    echo "$BOOT_ID" > "$MARK"

    # VINTF缓存失效 + 干净软重启（源码考证版，替代已废弃的servicemanager重启）：
    #  - libvintf 在每个进程内的 manifest 缓存以 /apex/apex-info.xml 的 mtime 为
    #    失效键（system/libvintf Apex.cpp GetModifiedTime + VintfObject.cpp Get()）。
    #    这是 APEX 在线更新的生产机制：touch 该文件，servicemanager 下一次
    #    isDeclared 查询即重新读盘（含我们的 overlay 声明），无需重启任何服务。
    #  - 随后 ctl.restart zygote（本机实证干净）：新 system_server 的 GnssHal 构造
    #    走 AIDL 路径：isDeclared=YES + 服务已注册 + 特性已声明 → gps provider。
    sh "$(dirname "$0")/clear_rescue.sh" >/dev/null 2>&1
    sh "$(dirname "$0")/inject_feature.sh" apply >/dev/null 2>&1
    refresh_vintf_cache

    echo "[i] 执行 zygote 软重启（带验证回退）..."
    setsid sh -c '
      exec > /data/adb/gnss_usb/survive.log 2>&1
      echo "=== $(date) touch+zygote 流程 ==="
      echo "pre: daemon=[$(pgrep -f gnss_usb_hal | tr "\n" " ")] svc=$(timeout 5 service list 2>/dev/null | grep -c android.hardware.gnss.IGnss)"
      Z0="$(pidof zygote64 2>/dev/null)$(pidof zygote 2>/dev/null)"
      setprop ctl.restart zygote
      sleep 5
      Z1="$(pidof zygote64 2>/dev/null)$(pidof zygote 2>/dev/null)"
      if [ "$Z0" = "$Z1" ]; then
          echo "[fallback] ctl.restart 未生效（zygote pid 未变），单次 kill（计数1/4，安全）"
          kill $(pidof zygote64) $(pidof zygote) 2>/dev/null
          sleep 5
          Z2="$(pidof zygote64 2>/dev/null)$(pidof zygote 2>/dev/null)"
          [ "$Z1" = "$Z2" ] && echo "[!] kill 也未生效——请手动点KSU管理器的软重启按钮"
      else
          echo "[OK] ctl.restart 生效（zygote pid: $Z0 -> $Z1）"
      fi
      i=0
      while [ $i -lt 24 ]; do
          sleep 5
          i=$((i+1))
          echo "t+${i}x5s: daemon=[$(pgrep -f gnss_usb_hal | tr "\n" " ")] svc=$(timeout 5 service list 2>/dev/null | grep -c android.hardware.gnss.IGnss) sm_pid=$(pidof servicemanager) boot=$(getprop sys.boot_completed)"
          timeout 8 logcat -d -t 3000 2>/dev/null | grep -iE "Reloading VINTF|in device VINTF manifest|GnssJni|VINTF parse error|Successfully got GNSS|gnss_usb" | sed "s/^/  LC /"
      done
    ' >/dev/null 2>&1 &
    sleep 1
}

case "$cmd" in
stop)
    stop_all
    exit 0
    ;;
status)
    pgrep -f "$BIN" >/dev/null 2>&1 && echo "daemon: RUNNING" || echo "daemon: NOT RUNNING"
    service list 2>/dev/null | grep gnss || echo "service: NOT REGISTERED"
    dumpsys location 2>/dev/null | grep -m3 "gps provider" || echo "gps provider: not visible"
    exit 0
    ;;
esac

start_daemon || exit 1

if [ "$cmd" = "soft" ]; then
    soft_reboot
    echo "提示：重启完成后可用  su -c sh $DEST/activate.sh status  查看结果"
    exit 0
fi

echo "---- gps provider 检查（若未出现，建议加 soft 参数做软重启）----"
sleep 3
if dumpsys location 2>/dev/null | grep -q "gps provider"; then
    dumpsys location 2>/dev/null | grep -m3 "gps provider"
    echo "[OK] framework 已接受 gps provider"
else
    echo "[WARN] 未见 gps provider —— 执行软重启后再看："
    echo "       su -c sh $DEST/activate.sh soft"
fi

echo "---- 提示 ----"
echo "卫星/定位确认: GPS Test 类App 或地图App"
echo "完整自检: su -c sh $DEST/diagnose.sh"
