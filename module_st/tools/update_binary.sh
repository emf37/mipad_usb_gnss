#!/system/bin/sh
# 安全更新运行中的HAL二进制：杀supervisor+daemon → cp → 重启daemon(注册) → ctl.restart zygote
# 全程<10s，赶在 android.fg 30s watchdog 窗口内；新system_server启动时守护已注册
DEST=/data/adb/gnss_usb
NEW=/data/local/tmp/gnss_usb_hal
NEWACT=/data/local/tmp/activate.sh
[ -f "$NEW" ] || { echo "! 先push新二进制"; exit 1; }
kill "$(cat $DEST/supervisor.pid 2>/dev/null)" 2>/dev/null
pkill -f gnss_usb_hal 2>/dev/null
sleep 1
cp "$NEW" "$DEST/bin/gnss_usb_hal" && chmod 755 "$DEST/bin/gnss_usb_hal" || { echo "! cp失败"; exit 1; }
[ -f "$NEWACT" ] && cp "$NEWACT" "$DEST/activate.sh" && chmod 755 "$DEST/activate.sh"
rm -f "$DEST/supervisor.pid" "$DEST/action.lock"
sh "$DEST/activate.sh" start >/dev/null 2>&1
sleep 4
timeout 8 service list | grep -q android.hardware.gnss.IGnss && echo "daemon已注册" || echo "!daemon未注册"
setprop ctl.restart zygote
echo "zygote重启已触发"
