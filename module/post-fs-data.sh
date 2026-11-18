#!/system/bin/sh
# Start the GNSS HAL daemon early (before system_server binds the GNSS HAL),
# supervised so it restarts if it ever exits.
MODDIR=${0%/*}
BIN="$MODDIR/bin/gnss_usb_hal"
LOG="$MODDIR/hal.log"
PIDFILE="$MODDIR/supervisor.pid"

# Respect a kill-switch property.
[ "$(getprop persist.vendor.gnss_usb.enabled)" = "0" ] && exit 0

# Single supervisor guard.
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
