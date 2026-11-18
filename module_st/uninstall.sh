#!/system/bin/sh
# 卸载：停掉本模块与独立部署的守护进程，清理 /data/adb/gnss_usb。
MODDIR=${0%/*}
PIDFILE="$MODDIR/supervisor.pid"

[ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null
pkill -f "$MODDIR/bin/gnss_usb_hal" 2>/dev/null

# 独立部署副本属于本模块，一并清理
STPID=/data/adb/gnss_usb/supervisor.pid
[ -f "$STPID" ] && kill "$(cat "$STPID" 2>/dev/null)" 2>/dev/null
pkill -f /data/adb/gnss_usb/bin/gnss_usb_hal 2>/dev/null
rm -rf /data/adb/gnss_usb

rm -f "$MODDIR/hal.log" "$PIDFILE"
exit 0
