#!/usr/bin/env bash
# Build dropbear + scp for iOS 12 (arm64) into out/shell/build, stripped + ad-hoc signed.
# iOS has no setresgid(), so the server must not drop privileges; stream forwarding needs
# privilege dropping, so it is off too. Host keys are generated on the phone (-R).
set -euo pipefail
cd "$(dirname "$0")/.."
source "$(cd "$(dirname "$0")" && pwd)/env.sh"
op2_require_xcode || exit 1
SDK="$IPHONEOS_SDK"
SRC=out/shell/dropbear-2026.94
[ -d "$SRC" ] || { echo "missing $SRC (download dropbear-2026.94.tar.bz2 there)" >&2; exit 1; }

cat > "$SRC/localoptions.h" <<'EOF'
/* OnePlus 2 iOS port: iOS lacks setresgid(), so no privilege dropping; the
 * server runs as root on a USB-only lab link. Stream-forwarding options
 * require priv-dropping, so they are off too (unused here). */
#define DROPBEAR_SVR_DROP_PRIVS 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0
EOF

if [ ! -f "$SRC/Makefile" ]; then
  (cd "$SRC" && ./configure --host=aarch64-apple-darwin --disable-dependency-tracking \
    "CC=clang -target arm64-apple-ios12.0 -isysroot $SDK" CC_FOR_BUILD=clang CFLAGS=-O2 --disable-zlib)
fi
make -C "$SRC" -j8 dropbear scp
mkdir -p out/shell/build
for b in dropbear scp; do
  cp "$SRC/$b" "out/shell/build/$b"
  strip -x "out/shell/build/$b"
  codesign -s - --force "out/shell/build/$b"
done
ls -l out/shell/build/dropbear out/shell/build/scp
file out/shell/build/dropbear out/shell/build/scp
