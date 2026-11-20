#!/system/bin/sh
# KSU/Magisk 安装期脚本：
# 1) 部署独立版 HAL 到 /data/adb/gnss_usb（不依赖 magic mount）
# 2) 完成与【执行操作】相同的全部安全动作：启动守护 + 特性/VINTF注入 + 体检
#    （不做任何软重启——部分管理器装完后不显示执行操作按钮，需软重启后才出现，
#     因此安装期把能做的全部做完）
STANDALONE=/data/adb/gnss_usb

ui_print "- deploying standalone HAL to $STANDALONE"
mkdir -p "$STANDALONE/bin" || abort "! cannot create $STANDALONE"

cp -f "$MODPATH/bin/gnss_usb_hal" "$STANDALONE/bin/gnss_usb_hal"
chmod 755 "$STANDALONE/bin/gnss_usb_hal"
for f in activate.sh diagnose.sh inject_feature.sh clear_rescue.sh suspend_test.sh; do
    [ -f "$MODPATH/tools/$f" ] && cp -f "$MODPATH/tools/$f" "$STANDALONE/$f"
done
chmod 755 "$STANDALONE"/*.sh 2>/dev/null

# 波特率默认115200（本机模组实测），未显式设置时锁定
[ -n "$(getprop persist.vendor.gnss_usb.baud)" ] || setprop persist.vendor.gnss_usb.baud 115200

# 停掉旧实例（宽松匹配含孤儿 supervisor），清防连点锁，setsid 脱离安装进程树
STPID="$STANDALONE/supervisor.pid"
[ -f "$STPID" ] && kill "$(cat "$STPID" 2>/dev/null)" 2>/dev/null
pkill -f gnss_usb_hal 2>/dev/null
rm -f "$STANDALONE/action.lock"
sleep 1
setsid sh -c '
  echo $$ > "'"$STANDALONE"'/supervisor.pid"
  cd "'"$STANDALONE"'"
  while true; do
    [ "$(cat "'"$STANDALONE"'/supervisor.pid" 2>/dev/null)" = "$$" ] || exit 0
    "'"$STANDALONE"'/bin/gnss_usb_hal" >> "'"$STANDALONE"'/hal.log" 2>&1
    sleep 3
  done
' >/dev/null 2>&1 &
sleep 3

if timeout 15 service list 2>/dev/null | grep -q "android.hardware.gnss.IGnss"; then
    service list 2>/dev/null | grep gnss | while read -r l; do ui_print "[OK] $l"; done
else
    ui_print "[!] 服务未注册，hal.log 尾部："
    tail -3 "$STANDALONE/hal.log" 2>/dev/null | while read -r l; do ui_print "    $l"; done
    ui_print "（反馈 avc 日志：dmesg | grep avc）"
fi

ui_print "- 注入 FEATURE_LOCATION_GPS / VINTF 声明..."
sh "$STANDALONE/inject_feature.sh" apply 2>&1 | while read -r l; do ui_print "    $l"; done

# 安装时也清一次 RescueParty，防止历史崩溃计数在后续重启时升级自救
if [ -f "$STANDALONE/clear_rescue.sh" ]; then
    sh "$STANDALONE/clear_rescue.sh" >/dev/null 2>&1
    ui_print "[i] RescueParty 已清零并禁用"
fi

PROVIDER=no
timeout 15 dumpsys location 2>/dev/null | grep -qiE "gps.*(provider|enabled)" && PROVIDER=yes
if timeout 10 pm has-feature android.hardware.location.gps 2>/dev/null; then
    ui_print "[i] feature=yes provider=$PROVIDER（feature 已生效；provider 需下次软重启后由新 system_server 创建）"
else
    ui_print "[i] feature=no provider=$PROVIDER（feature 需软重启后生效）"
fi

ui_print " "
ui_print "  注意：当前为崩溃排查保守模式 —— HAL 静默（不推送任何回调），"
ui_print "  不会自动软重启。装完状态已就绪，无依赖【执行操作】按钮的步骤。"
ui_print " "
ui_print "  下一步取证：su -c sh $STANDALONE/diagnose.sh"
ui_print "  （以后重启后：激活root→点【执行操作】或 su -c sh $STANDALONE/activate.sh）"
