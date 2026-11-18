#!/system/bin/sh
# 半持久越狱（重启丢root）模式安装：把 HAL 放到 /data/adb/gnss_usb（真实文件，
# 不依赖 magic mount）。每次重启→激活root→跑 activate.sh 即可恢复 GNSS。
#
# 用法（把整个 dist/ 或仓库推到平板后）：
#   su -c sh /path/to/install_standalone.sh
set -e

SRC="$(dirname "$0")"
DEST=/data/adb/gnss_usb

mkdir -p "$DEST/bin"
cp "$SRC/../module/bin/gnss_usb_hal" "$DEST/bin/gnss_usb_hal" 2>/dev/null \
  || cp "$SRC/bin/gnss_usb_hal" "$DEST/bin/gnss_usb_hal"
chmod 755 "$DEST/bin/gnss_usb_hal"
cp "$SRC/activate.sh" "$DEST/activate.sh"
chmod 755 "$DEST/activate.sh"
cp "$SRC/diagnose.sh" "$DEST/diagnose.sh" 2>/dev/null || true

# 配置属性按需 setprop（默认 usb / 1a86:7523 / 9600，见 README）

echo "installed to $DEST"
echo "each boot: 激活root后执行  su -c sh $DEST/activate.sh"
