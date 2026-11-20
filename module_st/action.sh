#!/system/bin/sh
# KSU 管理器“执行操作”按钮回调（action.sh）——日常一键激活 GNSS。
# 流程：启动守护 → 特性/VINTF叠加注入 → 体检 → 需要时自动软重启。
# persist.vendor.gnss_usb.auto_soft=0 关闭自动软重启。
command -v ui_print >/dev/null 2>&1 || ui_print() { echo "$1"; }

STANDALONE=/data/adb/gnss_usb
BIN="$STANDALONE/bin/gnss_usb_hal"
[ -x "$BIN" ] || { ui_print "! 未找到 $BIN，请先重装模块"; exit 1; }

# 同步工具脚本
MODDIR="${0%/*}"
for f in activate.sh diagnose.sh inject_feature.sh clear_rescue.sh suspend_test.sh; do
    [ -f "$MODDIR/tools/$f" ] && cp -f "$MODDIR/tools/$f" "$STANDALONE/$f"
done
chmod 755 "$STANDALONE"/*.sh 2>/dev/null

# 无条件清 RescueParty：无论走到哪个分支，崩溃计数都不能遗留
# （计数累积会在任意重启时升级自救级别→重置设置/进recovery）
if [ -f "$STANDALONE/clear_rescue.sh" ]; then
    sh "$STANDALONE/clear_rescue.sh" >/dev/null 2>&1
    ui_print "[i] RescueParty 已清零并禁用"
fi

# 0) 防连点锁：120 秒内的重复点击直接短路
LOCK="$STANDALONE/action.lock"
NOW=$(date +%s)
if [ -f "$LOCK" ]; then
    LAST=$(cat "$LOCK" 2>/dev/null)
    if [ -n "$LAST" ] && [ $((NOW - LAST)) -lt 120 ]; then
        ui_print "[i] 刚执行过（<120s），忽略本次点击，防止重复软重启"
        exit 0
    fi
fi
echo $NOW > "$LOCK"

# 1) 健康短路：一切正常时只报状态，不重启守护、不软重启
if timeout 15 service list 2>/dev/null | grep -q "android.hardware.gnss.IGnss"; then
    PROVIDER=no
    timeout 15 dumpsys location 2>/dev/null | grep -qiE "gps.*(provider|enabled)" && PROVIDER=yes
    FEATURE=$(timeout 10 pm has-feature android.hardware.location.gps 2>/dev/null && echo yes || echo no)
    if [ "$PROVIDER" = "yes" ] && [ "$FEATURE" = "yes" ]; then
        ui_print "[OK] GNSS 正常工作中（服务已注册/provider就绪/feature就绪），无需操作"
        exit 0
    fi
fi

# 2) （重）启动守护进程：先清干净所有旧实例（supervisor + daemon，宽松匹配），
#    supervisor 带 pidfile 自检，被接管后自动退出，杜绝多实例
STPID="$STANDALONE/supervisor.pid"
[ -f "$STPID" ] && kill "$(cat "$STPID" 2>/dev/null)" 2>/dev/null
pkill -f gnss_usb_hal 2>/dev/null
sleep 1
setsid sh -c '
  echo $$ > "'"$STANDALONE"'/supervisor.pid"
  cd "'"$STANDALONE"'"
  while true; do
    [ "$(cat "'"$STANDALONE"'/supervisor.pid" 2>/dev/null)" = "$$" ] || exit 0
    "'"$BIN"'" >> "'"$STANDALONE"'/hal.log" 2>&1
    sleep 3
  done
' >/dev/null 2>&1 &
sleep 3

if timeout 15 service list 2>/dev/null | grep -q "android.hardware.gnss.IGnss"; then
    ui_print "[OK] IGnss/default 已注册"
else
    ui_print "[!] 服务未注册："
    tail -5 "$STANDALONE/hal.log" 2>/dev/null | while read -r l; do ui_print "    $l"; done
    ui_print "请反馈 avc 日志：dmesg | grep avc"
    exit 1
fi

# 2) 特性/VINTF 叠加注入（幂等；软重启后由新 system_server 读取）
ui_print "- 注入 FEATURE_LOCATION_GPS / VINTF 声明..."
sh "$STANDALONE/inject_feature.sh" apply 2>&1 | while read -r l; do ui_print "    $l"; done

# 3) 体检：provider 与 feature
sleep 3
PROVIDER=no
timeout 15 dumpsys location 2>/dev/null | grep -qiE "gps.*(provider|enabled)" && PROVIDER=yes
FEATURE=$(timeout 10 pm has-feature android.hardware.location.gps 2>/dev/null && echo yes || echo no)

if [ "$PROVIDER" = "yes" ] && [ "$FEATURE" = "yes" ]; then
    ui_print "[OK] gps provider + FEATURE 均就绪 —— GNSS 可用"
    exit 0
fi
ui_print "[i] provider=$PROVIDER feature=$FEATURE（feature 需软重启后新 system_server 读取）"

# 自动软重启默认开启（回调分级已全部实测稳定）：
# 【执行操作】= 完整激活流程：清RescueParty → 守护 → 注入 → VINTF缓存刷新 → zygote软重启
# 不想自动重启时：setprop persist.vendor.gnss_usb.auto_soft 0
AUTO_SOFT="$(getprop persist.vendor.gnss_usb.auto_soft)"
if [ "$AUTO_SOFT" = "0" ]; then
    ui_print "[i] 自动软重启已关闭（auto_soft=0）。"
    ui_print "    状态: provider=$PROVIDER feature=$FEATURE"
    ui_print "    手动软重启: su -c sh $STANDALONE/activate.sh soft"
    exit 0
fi

# 4) VINTF缓存刷新（bind-mount接管失效键）+ zygote软重启
sh "$STANDALONE/clear_rescue.sh" >/dev/null 2>&1
nsenter -t 1 -m -- umount /apex/apex-info.xml 2>/dev/null
STG_AI="$STANDALONE/stage-apex-info-list.xml"
cp /apex/apex-info-list.xml "$STG_AI" 2>/dev/null || : > "$STG_AI"
touch "$STG_AI"; chmod 644 "$STG_AI"
LBL=$(ls -Z /apex 2>/dev/null | awk 'NR==2{print $1}')
[ -n "$LBL" ] && chcon "$LBL" "$STG_AI" 2>/dev/null
if timeout 5 mount 2>/dev/null | grep -q " on /apex/apex-info-list.xml "; then
    touch "$STG_AI" && ui_print "    [OK] VINTF缓存已刷新（新mtime）"
elif nsenter -t 1 -m -- mount --bind "$STG_AI" /apex/apex-info-list.xml 2>/dev/null \
     || mount --bind "$STG_AI" /apex/apex-info-list.xml 2>/dev/null; then
    ui_print "    [OK] VINTF失效键已接管（apex-info-list.xml，新mtime）"
else
    ui_print "    [!] apex-info-list 覆盖失败"
fi
ui_print "    3 秒后 zygote 软重启（约60~90秒回来）"
sleep 3
setprop ctl.restart zygote
ui_print "[OK] 软重启已触发"
exit 0
