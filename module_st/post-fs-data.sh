#!/system/bin/sh
# 持久root环境下兜底自启（半持久模式下本脚本不会运行，需手动 activate.sh）。
MODDIR=${0%/*}
BIN="$MODDIR/bin/gnss_usb_hal"
LOG="$MODDIR/hal.log"
PIDFILE="$MODDIR/supervisor.pid"

# 让位：原 gnss_usb 模块启用时不自启，避免双守护。
ORIG=/data/adb/modules/gnss_usb
if [ -d "$ORIG" ] && [ ! -f "$ORIG/disable" ] && [ ! -f "$ORIG/remove" ]; then
    exit 0
fi

[ "$(getprop persist.vendor.gnss_usb.enabled)" = "0" ] && exit 0

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
    exit 0
fi

sh -c '
  echo $$ > "'"$PIDFILE"'"
  cd "'"$MODDIR"'"
  sleep 2
  : > "'"$LOG"'"
  chmod 644 "'"$LOG"'"
  while true; do
    "'"$BIN"'" >> "'"$LOG"'" 2>&1
    sleep 5
  done
' &
exit 0
