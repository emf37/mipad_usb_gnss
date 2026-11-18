#!/usr/bin/env bash
# fetch_deps.sh - download all build dependencies via local proxy 127.0.0.1:7890
# Downloads land in downloads/ (untrusted), extracts into deps/<name>/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PROXY="http://127.0.0.1:7890"
DL="$ROOT/downloads"
DEPS="$ROOT/deps"
mkdir -p "$DL" "$DEPS"

fetch() { # fetch <url> <outfile>
  local url="$1" out="$DL/$2"
  if [[ -s "$out" ]]; then echo "[skip] $2 exists"; return 0; fi
  echo "[get ] $2"
  curl -fL --retry 3 --connect-timeout 20 -x "$PROXY" -o "$out.part" "$url"
  mv "$out.part" "$out"
}

unzip_to() { # unzip_to <zip> <dest-dir>
  local zip="$1" dest="$DEPS/$2"
  if [[ -d "$dest" ]]; then echo "[skip] $2 extracted"; return 0; fi
  mkdir -p "$dest"
  if command -v unzip >/dev/null 2>&1; then
    unzip -q "$zip" -d "$dest"
  else
    /c/Windows/System32/tar.exe -xf "$(cygpath -w "$zip")" -C "$(cygpath -w "$dest")"
  fi
}

untgz_to() { # untgz_to <tgz> <dest-dir>
  local tgz="$1" dest="$DEPS/$2"
  if [[ -d "$dest" ]]; then echo "[skip] $2 extracted"; return 0; fi
  mkdir -p "$dest"
  tar -xzf "$tgz" -C "$dest"
}

case "${1:-all}" in
ndk)
  # NDK r27c (before r28 min-sdk regression for generated NdkBinder code)
  fetch "https://dl.google.com/android/repository/android-ndk-r27c-windows.zip" "ndk-r27c.zip"
  ;;
build-tools)
  for v in 35.0.0 34.0.0; do
    if fetch "https://dl.google.com/android/repository/build-tools_r${v%%.*}-windows.zip" "build-tools-${v}.zip"; then
      unzip_to "$DL/build-tools-${v}.zip" "build-tools-${v}"
      # normalize: strip nested android-XX dir
      break
    fi
  done
  ;;
interfaces)
  # AOSP hardware/interfaces via gitiles +archive (only needed subtrees)
  fetch "https://android.googlesource.com/platform/hardware/interfaces/+archive/refs/heads/main/gnss/aidl.tar.gz" "gnss-aidl-src.tar.gz"
  fetch "https://android.googlesource.com/platform/hardware/interfaces/+archive/refs/heads/main/common/aidl.tar.gz" "common-aidl-src.tar.gz"
  mkdir -p "$DEPS/hardware-interfaces/gnss" "$DEPS/hardware-interfaces/common"
  # +archive tarballs have no top dir
  rm -rf "$DEPS/hardware-interfaces/gnss/aidl" "$DEPS/hardware-interfaces/common/aidl"
  mkdir -p "$DEPS/hardware-interfaces/gnss/aidl" "$DEPS/hardware-interfaces/common/aidl"
  tar -xzf "$DL/gnss-aidl-src.tar.gz" -C "$DEPS/hardware-interfaces/gnss/aidl"
  tar -xzf "$DL/common-aidl-src.tar.gz" -C "$DEPS/hardware-interfaces/common/aidl"
  ;;
gnss-aidl-serial)
  # Porting base: AIDL GNSS HAL V4 for serial NMEA receivers (verified on Android 17)
  fetch "https://github.com/aselafernando/gnss-aidl-serial/archive/refs/heads/main.tar.gz" "gnss-aidl-serial.tar.gz"
  untgz_to "$DL/gnss-aidl-serial.tar.gz" "gnss-aidl-serial"
  ;;
usb-serial-for-android)
  # Apache-2.0 reference for CH340 userspace control-transfer sequence (Ch34xSerialDriver)
  fetch "https://github.com/mik3y/usb-serial-for-android/archive/refs/heads/master.tar.gz" "usb-serial-for-android.tar.gz"
  untgz_to "$DL/usb-serial-for-android.tar.gz" "usb-serial-for-android"
  ;;
platform)
  # android.jar for javac/d8 (bridge APK build)
  fetch "https://dl.google.com/android/repository/platform-35_r01.zip" "platform-35.zip"
  unzip_to "$DL/platform-35.zip" "platform-35"
  ;;
all)
  "$0" interfaces
  "$0" gnss-aidl-serial
  "$0" usb-serial-for-android
  "$0" build-tools
  "$0" platform
  echo "[hint] run '$0 ndk' separately (~1.4GB download)"
  ;;
*)
  echo "usage: $0 [all|ndk|build-tools|interfaces|gnss-aidl-serial|usb-serial-for-android|platform]"
  ;;
esac
echo "[done] $1"
