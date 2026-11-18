#!/usr/bin/env bash
# build_hal.sh - regenerate AIDL bindings, compile the HAL with the NDK and
# package the KernelSU module into dist/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
AIDL="$ROOT/deps/build-tools-34.0.0/android-14/aidl.exe"
NDK="$ROOT/deps/ndk/r27c"
V4_HASH="3552d3a7d21e743a59aa62eb395be44dddc0797e"
VERSION="$(sed -n 's/^version=//p' "$ROOT/module/module.prop")"
OUT="$ROOT/dist"

echo "== [1/4] AIDL codegen (gnss V4, NDK backend) =="
rm -rf "$ROOT/hal/generated"
mkdir -p "$ROOT/hal/generated/include" "$ROOT/hal/generated/src"
"$AIDL" --lang=ndk --structured --stability=vintf --version=4 --hash="$V4_HASH" \
  -I "$ROOT/hal/aidl" \
  -h "$ROOT/hal/generated/include" -o "$ROOT/hal/generated/src" \
  "$ROOT"/hal/aidl/android/hardware/gnss/*.aidl \
  "$ROOT"/hal/aidl/android/hardware/gnss/measurement_corrections/*.aidl \
  "$ROOT"/hal/aidl/android/hardware/gnss/visibility_control/*.aidl

echo "== [2/4] NDK build (arm64-v8a) =="
BUILD="$ROOT/hal/build-arm64"
mkdir -p "$BUILD"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
CXX="$TOOLCHAIN/clang++.exe"

# Direct clang++ invocation — no cmake/ninja dependency. The --target triple
# selects the NDK sysroot (bionic) automatically.
SRCS="$ROOT"/hal/src/*.cpp
GEN_SRCS=$(find "$ROOT/hal/generated/src" -name '*.cpp')

# shellcheck disable=SC2086
"$CXX" --target=aarch64-linux-android30 -std=c++17 -O2 -Wall -Wextra \
  -I"$ROOT/hal/src/shim" -I"$ROOT/hal/generated/include" -I"$ROOT/hal/src" \
  $SRCS $GEN_SRCS \
  -o "$BUILD/gnss_usb_hal" \
  -static-libstdc++ -lbinder_ndk -llog

"$TOOLCHAIN/llvm-readelf.exe" -h "$BUILD/gnss_usb_hal" | grep -E "Class|Machine|Type"

echo "== [3/4] assemble modules =="
install -m 0755 "$BUILD/gnss_usb_hal" "$ROOT/module/bin/gnss_usb_hal"
install -m 0755 "$BUILD/gnss_usb_hal" "$ROOT/module_st/bin/gnss_usb_hal"
# keep the semi-tethered module's bundled tools in sync
cp -f "$ROOT/tools/activate.sh" "$ROOT/tools/diagnose.sh" "$ROOT/tools/inject_feature.sh" "$ROOT/module_st/tools/"
mkdir -p "$OUT"
python "$(dirname "$0")/tools/pack_module.py" module
python "$(dirname "$0")/tools/pack_module.py" module_st

echo "== [4/4] done =="
ls -la "$OUT"
