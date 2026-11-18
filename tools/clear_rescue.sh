#!/system/bin/sh
# 清除并禁用 RescueParty（Android 连环崩溃自救机制）。
# 之前的 system_server 崩溃把计数存进持久属性/设置里，累积后软重启会误触发
# 更高级别自救（重置设置/进recovery）。本脚本清零计数并关闭该机制。
#
# 用法：su -c sh /data/adb/gnss_usb/clear_rescue.sh

echo "--- before ---"
getprop persist.sys.rescue_level
getprop persist.sys.enable_rescue_party
settings list global 2>/dev/null | grep -i rescue || echo "(no rescue settings)"

# 禁用 + 级别清零（多属性加厚：AOSP门控 + PackageWatchdog观测 + 级别）
setprop persist.sys.enable_rescue_party false
setprop persist.sys.enable_package_watchdog false
setprop persist.sys.rescue_level 0

# 清掉所有 rescue 相关的全局设置项（计数/状态）
for k in $(settings list global 2>/dev/null | grep -i rescue | cut -d= -f1); do
    settings delete global "$k" 2>/dev/null
done
# 常见键兜底（删不掉就置0）
settings put global rescue_boot_count 0 2>/dev/null

# PackageWatchdog（RescueParty 的观测源）状态一并重置
for k in $(settings list global 2>/dev/null | grep -i watchdog | cut -d= -f1); do
    settings delete global "$k" 2>/dev/null
done

echo "--- after ---"
echo "rescue_level=$(getprop persist.sys.rescue_level)"
echo "enable_rescue_party=$(getprop persist.sys.enable_rescue_party)"
settings list global 2>/dev/null | grep -iE "rescue|watchdog" || echo "(clean)"
echo "[OK] RescueParty 已清零并禁用（persist.sys.enable_rescue_party=false）"
