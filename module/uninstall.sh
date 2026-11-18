#!/system/bin/sh
# Remove runtime artifacts when the module is deleted.
MODDIR=${0%/*}
PIDFILE="$MODDIR/supervisor.pid"
if [ -f "$PIDFILE" ]; then
    kill "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null
    rm -f "$PIDFILE"
fi
pkill -f "$MODDIR/bin/gnss_usb_hal" >/dev/null 2>&1
rm -f "$MODDIR/hal.log"
exit 0
