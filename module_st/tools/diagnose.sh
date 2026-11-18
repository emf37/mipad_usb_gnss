#!/system/bin/sh
# gnss_usb 一键自检 v2：自动探测安装位置（standalone / gnss_usb_st / gnss_usb），
# 重点判定 gps provider 是否创建、是否有定位产出。
OUT=/sdcard/gnss_usb_diagnose.txt

section() { echo; echo "======== $1 ========"; }

# --- 定位安装目录 ---
HALDIR=""
for d in /data/adb/gnss_usb /data/adb/modules/gnss_usb_st /data/adb/modules/gnss_usb; do
    if [ -x "$d/bin/gnss_usb_hal" ]; then HALDIR="$d"; break; fi
done

{
section "device"
getprop ro.product.model
getprop ro.build.version.release
getprop ro.build.version.sdk
uname -r

section "install dir"
echo "HALDIR=$HALDIR"
[ -n "$HALDIR" ] && ls -la "$HALDIR" "$HALDIR/bin" 2>&1

section "daemon"
pgrep -f gnss_usb_hal >/dev/null 2>&1 \
    && echo "daemon: RUNNING (pid $(pgrep -f gnss_usb_hal | tr '\n' ' '))" \
    || echo "daemon: NOT RUNNING"
if [ -n "$HALDIR" ]; then
    echo "--- hal.log (tail 40) ---"
    tail -40 "$HALDIR/hal.log" 2>&1
fi

section "binder service"
timeout 15 service list 2>/dev/null | grep -i gnss || echo "no gnss binder service registered"
echo "--- raw binder probe: service call close(txn 2, 无参) ---"
timeout 10 service call android.hardware.gnss.IGnss/default 2 2>&1 | head -3
echo "(若 HAL 能接客, logcat 会出现 'framework call: close')"

section "framework location providers (full)"
timeout 15 dumpsys location 2>/dev/null | grep -iE "provider|gnss" | head -60

section "gps provider verdict"
if dumpsys location 2>/dev/null | grep -qiE "gps.*(enabled|provider)|provider.*gps"; then
    echo "VERDICT: gps provider EXISTS"
    dumpsys location 2>/dev/null | grep -iE "gps" | head -20
else
    echo "VERDICT: gps provider NOT FOUND in dumpsys"
fi

section "last locations"
timeout 15 dumpsys location 2>/dev/null | grep -i "last location" | head -6

section "system features (location+wifi)"
timeout 10 pm list features 2>/dev/null | grep -iE "location|wifi"
echo "--- fingerprint feature ---"
timeout 10 pm list features 2>/dev/null | grep -i fingerprint || echo "FINGERPRINT FEATURE MISSING"
echo "--- full feature count ---"
timeout 10 pm list features 2>/dev/null | wc -l

section "overlay mount state"
timeout 10 mount 2>/dev/null | grep -iE "vendor/etc" || echo "no overlay mounted"
echo "--- live /vendor/etc/permissions count+key files ---"
timeout 5 ls /vendor/etc/permissions/ 2>/dev/null | wc -l
timeout 5 ls /vendor/etc/permissions/ 2>/dev/null | grep -iE "fingerprint|wifi|gps" || echo "(none)"

section "stage dir (full)"
timeout 5 ls /data/adb/gnss_usb/stage/permissions/ 2>/dev/null | head -40
timeout 5 ls /data/adb/gnss_usb/stage/permissions/ 2>/dev/null | wc -l

section "rescue props alive?"
getprop persist.sys.enable_rescue_party
getprop persist.sys.rescue_level
getprop persist.sys.enable_package_watchdog

section "vintf overlay visibility"
timeout 5 ls -la /vendor/etc/vintf/manifest/ 2>&1 | head -30
echo "--- gnss fragment content ---"
timeout 5 cat /vendor/etc/vintf/manifest/android.hardware.gnss-usb.xml 2>&1
echo "--- checkvintf ---"
timeout 30 checkvintf 2>&1 | head -10

section "usb device scan"
for d in /sys/bus/usb/devices/*; do
    case "$d" in *:*) continue ;; esac
    v=$(cat "$d/idVendor" 2>/dev/null); p=$(cat "$d/idProduct" 2>/dev/null)
    [ -n "$v" ] && echo "$d vid=$v pid=$p bus=$(cat "$d/busnum" 2>/dev/null) dev=$(cat "$d/devnum" 2>/dev/null)"
done
echo "--- usbfs node ---"
ls -l /dev/bus/usb/*/* 2>&1 | head -10

section "dropbox crash records (list)"
timeout 15 dumpsys dropbox 2>/dev/null | tail -25
echo "--- newest system_server_crash stack (burst start) ---"
NEWEST_CRASH=$(timeout 15 dumpsys dropbox 2>/dev/null | grep "system_server_crash" | tail -1 | awk '{print $1, $2}')
echo "newest: $NEWEST_CRASH"
if [ -n "$NEWEST_CRASH" ]; then
    # 回溯几秒抓爆发点第一条；时间过滤不可靠时用全量打印取尾部兜底
    timeout 25 dumpsys dropbox --print ${NEWEST_CRASH%:*}":00" system_server_crash 2>/dev/null \
        | sed -n '/system_server_crash/,/^$/p' | tail -80
fi
echo "--- fallback: full print tail ---"
timeout 40 dumpsys dropbox --print system_server_crash 2>/dev/null | tail -100

section "tombstones"
ls -la /data/tombstones/ 2>&1 | tail -8
for t in $(ls -t /data/tombstones/tombstone_* 2>/dev/null | head -2); do
    echo "== $t (head 50) =="
    head -50 "$t" 2>&1
done

section "selinux"
getenforce
echo "--- avc denials last 30 (all) ---"
timeout 10 dmesg 2>/dev/null | grep "avc:  denied" | tail -30

section "logcat gnss/vintf-related (last 150)"
timeout 20 logcat -d 2>/dev/null | grep -iE "gnss_usb|IGnss|GnssLocationProvider|GnssManagerService|GnssNative|GnssJni|libvintf|vintf" | tail -150

section "props"
getprop | grep gnss_usb
} 2>&1 | tee "$OUT"

echo
echo "saved to $OUT"
echo "建议测试顺序："
echo " 1) 打开 GPS Test / GnssLogger 等 App 并开始请求定位，再跑一次本脚本"
echo " 2) 点模块【执行操作】(或 activate.sh soft) 软重启后，再跑一次本脚本对比"
