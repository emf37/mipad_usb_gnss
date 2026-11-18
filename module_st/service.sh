#!/system/bin/sh
# late_start 兜底（仅持久root环境生效）。
MODDIR=${0%/*}
BIN="$MODDIR/bin/gnss_usb_hal"
LOG="$MODDIR/hal.log"
PIDFILE="$MODDIR/supervisor.pid"

ORIG=/data/adb/modules/gnss_usb
if [ -d "$ORIG" ] && [ ! -f "$ORIG/disable" ] && [ ! -f "$ORIG/remove" ]; then
    exit 0
fi

[ "$(getprop persist.vendor.gnss_usb.enabled)" = "0" ] && exit 0

pgrep -f "$BIN" >/dev/null 2>&1 && exit 0

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
    exit 0
fi

sh -c '
  echo $$ > "'"$PIDFILE"'"
  cd "'"$MODDIR"'"
  : > "'"$LOG"'"
  while true; do
    "'"$BIN"'" >> "'"$LOG"'" 2>&1
    sleep 5
  done
' &
exit 0
