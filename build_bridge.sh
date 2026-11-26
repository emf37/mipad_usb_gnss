#!/usr/bin/env bash
# build_bridge.sh - build bridge/GnssBtBridge.apk with the raw SDK toolchain
# (aapt2 + javac + d8 + zipalign + apksigner), no Gradle.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BT="$ROOT/deps/build-tools-34.0.0/android-14"
AJAR="$ROOT/deps/platform-35/android-35/android.jar"
JDK="/c/Program Files/Java/jdk-23/bin"
BUILD="$ROOT/bridge/build"
KEYSTORE="$ROOT/deps/bridge.keystore"

rm -rf "$BUILD"
mkdir -p "$BUILD/gen" "$BUILD/classes"

echo "== [1/5] aapt2 compile+link =="
"$BT/aapt2.exe" compile --dir "$ROOT/bridge/res" -o "$BUILD/res.zip"
"$BT/aapt2.exe" link -I "$AJAR" \
  --manifest "$ROOT/bridge/AndroidManifest.xml" \
  --java "$BUILD/gen" \
  -o "$BUILD/base.apk" \
  "$BUILD/res.zip"

echo "== [2/5] javac =="
SRCS=$(find "$ROOT/bridge/src" "$BUILD/gen" -name '*.java' 2>/dev/null)
# shellcheck disable=SC2086
"$JDK/javac.exe" --release 8 -nowarn -classpath "$AJAR" -d "$BUILD/classes" $SRCS

echo "== [3/5] d8 =="
CLS=$(find "$BUILD/classes" -name '*.class')
# shellcheck disable=SC2086
java -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 --release --lib "$AJAR" \
  --output "$BUILD" $CLS

echo "== [4/5] package + align =="
python - "$BUILD" <<'EOF'
import sys, zipfile, pathlib
build = pathlib.Path(sys.argv[1])
with zipfile.ZipFile(build / "base.apk", "a") as zf:
    zf.write(build / "classes.dex", "classes.dex")
EOF
"$BT/zipalign.exe" -f 4 "$BUILD/base.apk" "$BUILD/aligned.apk"

echo "== [5/5] sign =="
if [[ ! -f "$KEYSTORE" ]]; then
  "$JDK/keytool.exe" -genkeypair -keystore "$(cygpath -w "$KEYSTORE")" \
    -alias bridge -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass android -keypass android \
    -dname "CN=gnss_usb bridge,O=ksu-gnss,C=CN"
fi
java -cp "$BT/lib/apksigner.jar" com.android.apksigner.ApkSignerTool sign \
  --ks "$(cygpath -w "$KEYSTORE")" --ks-pass pass:android --key-pass pass:android \
  --out "$(cygpath -w "$ROOT/dist/GnssBtBridge-v2.7.apk")" \
  "$(cygpath -w "$BUILD/aligned.apk")"

ls -la "$ROOT/dist/GnssBtBridge-v2.7.apk"
