#!/usr/bin/env bash
# Build toybox for iOS 12 (arm64) into out/shell/build/toybox, stripped + ad-hoc signed.
# Needs GNU sed (brew install gnu-sed). The iOS SDK lacks sys/disk.h; out/shell/toybox-ios-shim covers it.
# Upstream stty (toys/pending) has no Darwin support and stays off.
set -euo pipefail
cd "$(dirname "$0")/.."
SDK=/Users/veerhooda/xcode-10.3/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS12.4.sdk
SRC=out/shell/toybox-0.8.12
[ -d "$SRC" ] || { echo "missing $SRC (see docs/USB-CONSOLE-PLAN.md stage 2)" >&2; exit 1; }
export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
APPLETS="LS CAT GREP OD WC ID UNAME FILE DF DU HEAD TAIL TEE TR BASENAME DIRNAME MKDIR RM CP MV LN TOUCH CHMOD"
(
  cd "$SRC"
  if [ ! -f .config ]; then
    make allnoconfig >/dev/null
    # shellcheck disable=SC2086
    for t in $APPLETS; do sed -i "s/# CONFIG_$t is not set/CONFIG_$t=y/" .config; done
    make olddefconfig >/dev/null
  fi
  sed -i "s/^CONFIG_STTY=y/# CONFIG_STTY is not set/" .config
  make -j8 CC=clang CFLAGS="-target arm64-apple-ios12.0 -isysroot $SDK -I$PWD/../toybox-ios-shim -O2"
)
mkdir -p out/shell/build
rm -f out/shell/build/toybox
cp "$SRC/toybox" out/shell/build/toybox
chmod u+w out/shell/build/toybox
strip -x out/shell/build/toybox
codesign -s - --force out/shell/build/toybox
ls -l out/shell/build/toybox
file out/shell/build/toybox
