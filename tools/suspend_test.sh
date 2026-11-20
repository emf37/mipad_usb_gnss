#!/system/bin/sh
# 息屏供电/唤醒连续性测试：验证平板休眠时 OTG 供电是否持续、唤醒后定位是否连续。
# 用法：插好模组、GNSS激活状态下运行：
#   su -c sh /data/adb/gnss_usb/tools/suspend_test.sh <息屏分钟数，默认10>
OUT=/sdcard/gnss_usb_suspend_test.txt
MINUTES="${1:-10}"

snap() {
    echo "--- $(date '+%H:%M:%S') scr=$(dumpsys power 2>/dev/null | grep -m1 'mWakefulness=' | cut -d= -f2) daemon=[$(pgrep -f gnss_usb_hal | tr '\n' ' ')] svc=$(timeout 5 service list 2>/dev/null | grep -c android.hardware.gnss.IGnss)"
    timeout 8 sh -c 'echo STATUS | nc 127.0.0.1 5001' 2>/dev/null || \
        (printf 'STATUS\n'; sleep 1) | timeout 5 toybox nc 127.0.0.1 5001 2>/dev/null | head -1
}

{
echo "=== 息屏供电测试 $(date) ==="
echo "息屏时长: ${MINUTES} 分钟"
snap
echo "[i] 现在请手动按电源键息屏，等 ${MINUTES} 分钟后唤醒"
} | tee "$OUT"

sleep $((MINUTES * 60))

{
echo "=== 唤醒后 $(date) ==="
snap
sleep 15
echo "=== 唤醒后15秒（观察fix恢复） ==="
snap
sleep 45
echo "=== 唤醒后60秒 ==="
snap
} | tee -a "$OUT"

echo "结果判读："
echo "  三次都有 sats>0 / fix=3 → 供电连续，导航息屏不断链 ✅"
echo "  唤醒后 sats=0 → 休眠断电/断链，唤醒需重新搜星（记录重捕时间）"
echo "  svc=0 → 守护进程被杀（root丢失或LMK），需要检查 supervisor"
echo "日志: $OUT"
