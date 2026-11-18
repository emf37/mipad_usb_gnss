#!/system/bin/sh
# late_start safety net: ensure the supervisor is alive after early boot.
MODDIR=${0%/*}
BIN="$MODDIR/bin/gnss_usb_hal"
LOG="$MODDIR/hal.log"
PIDFILE="$MODDIR/supervisor.pid"

[ "$(getprop persist.vendor.gnss_usb.enabled)" = "0" ] && exit 0

# Daemon alive? (supervisor is watching it)
pgrep -f "$BIN" >/dev/null 2>&1 && exit 0

# Supervisor alive? (it will respawn the daemon)
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
