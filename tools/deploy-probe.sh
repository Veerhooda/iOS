#!/usr/bin/env bash
# Put a userland GPU probe on the phone as /usr/libexec/tzinit (a launchd boot task, signed com.apple.tzinit),
# written in place over adb by tools/op2hfs.py (TWRP has no hfsplus). The file cannot grow: the probe is padded
# with zeros to tzinit's size. Phone must be in TWRP.
#   tools/deploy-probe.sh                 # userland/op2gpu/out/ios/op2gpu-probe
#   tools/deploy-probe.sh --restore       # the original tzinit back (backup/rootfs/tzinit.orig)
set -euo pipefail
cd "$(dirname "$0")/.."
ORIG=backup/rootfs/tzinit.orig
SIZE=112224
[ -f "$ORIG" ] || { echo "missing $ORIG (the original tzinit): refusing to overwrite it" >&2; exit 1; }
if [ "${1:-}" = "--restore" ]; then
  SRC="$ORIG"
else
  SRC="${1:-userland/op2gpu/out/ios/op2gpu-probe}"
fi
n=$(stat -f %z "$SRC")
[ "$n" -le "$SIZE" ] || { echo "$SRC is $n bytes; tzinit holds $SIZE" >&2; exit 1; }
TMP=$(mktemp); trap 'rm -f "$TMP"' EXIT
cp "$SRC" "$TMP"
[ "$n" -lt "$SIZE" ] && dd if=/dev/zero bs=1 count=$((SIZE - n)) >> "$TMP" 2>/dev/null
python3 -I tools/op2hfs.py write /usr/libexec/tzinit 0 "$TMP"
python3 -I tools/op2hfs.py read /usr/libexec/tzinit 0 $SIZE "$TMP.back" >/dev/null
cmp "$TMP" "$TMP.back" && echo "tzinit now = $SRC ($n bytes, padded to $SIZE), verified" ; rm -f "$TMP.back"
