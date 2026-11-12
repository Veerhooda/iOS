#!/usr/bin/env bash
# Build userland/op2shell (embeds the user-built dash) and install it IN PLACE over /usr/local/bin/op2-air-run
# (phone in TWRP). Usage: bash tools/build-op2shell.sh [--install]
set -euo pipefail
cd "$(dirname "$0")/.."
SDK=/Users/veerhooda/xcode-10.3/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS12.4.sdk
DASH=out/shell/dash-0.5.12/src/dash
[ -f "$DASH" ] || { echo "build dash first (out/shell/dash-0.5.12)" >&2; exit 1; }
mkdir -p out/shell/build
cp "$DASH" out/shell/build/dash && strip -x out/shell/build/dash && codesign -s - --force out/shell/build/dash
clang -target arm64-apple-ios12.0 -isysroot "$SDK" -O2 -Wall -Wextra \
  -DDASH_BIN="\"$PWD/out/shell/build/dash\"" -o out/shell/build/op2shell userland/op2shell/op2shell.c
strip -x out/shell/build/op2shell && codesign -s - -i com.op2.air-run --force out/shell/build/op2shell
ls -l out/shell/build/dash out/shell/build/op2shell
if [ "${1:-}" = "--install" ]; then
  SLOT=/usr/local/bin/op2-air-run
  SIZE=$(python3 -I tools/op2hfs.py ls /usr/local/bin >/dev/null && python3 -I -c "
import sys; sys.path.insert(0,'tools'); import op2hfs; print(op2hfs.resolve('$SLOT')[3])")
  n=$(stat -f %z out/shell/build/op2shell)
  [ "$n" -le "$SIZE" ] || { echo "op2shell $n > slot $SIZE" >&2; exit 1; }
  python3 -I -c "
import sys; d=open('out/shell/build/op2shell','rb').read(); open('out/shell/build/op2shell.padded','wb').write(d+b'\\0'*($SIZE-len(d)))"
  python3 -I tools/op2hfs.py write "$SLOT" 0 out/shell/build/op2shell.padded
  python3 -I tools/op2hfs.py read "$SLOT" 0 "$SIZE" out/shell/build/check >/dev/null
  cmp out/shell/build/check out/shell/build/op2shell.padded && echo "installed over $SLOT ($n bytes, slot $SIZE), verified"
fi
