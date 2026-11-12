#!/usr/bin/env bash
# Fresh, JOURNALED iOS rootfs from the last clean image, then flash it.
# Needs: phone in TWRP, sudo password (for file ownership on the image).
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=out/ios12-userdata-fixA.dmg
DST=out/ios12-userdata-r5.dmg
MNT=/tmp/op2root
[ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 1; }
echo "[1/5] copying clean image -> $DST"
rm -f "$DST"; cp -c "$SRC" "$DST" 2>/dev/null || cp "$SRC" "$DST"
echo "[2/5] mounting"
mkdir -p "$MNT"
hdiutil attach -nobrowse -owners on -mountpoint "$MNT" "$DST" >/dev/null
trap 'hdiutil detach "$MNT" -force >/dev/null 2>&1 || true' EXIT
echo "[3/5] enabling HFS+ journaling"
diskutil enableJournal "$MNT"
diskutil info "$MNT" | grep -i -E "journal|File System Personality" || true
echo "[4/5] seeding prefs"
sudo OP2_BLUR="${OP2_BLUR:-}" python3 tools/rootfs_prefs.py "$MNT"
# G3: the Mesa/freedreno draw test, when it has been built (tools/mesa-ios.sh)
if [ -f out/mesa-ios/out/air_run ]; then
  sudo python3 tools/rootfs_g3.py "$MNT"
fi
# G5: zero-padded slots for the Metal driver library and a growable OP2Metal (later updates go in place)
sudo python3 tools/rootfs_g5.py "$MNT"
# games: every out/games/build/*.app into /Applications (tools/games/build-*.sh)
sudo python3 tools/rootfs_games.py "$MNT"
sync
hdiutil detach "$MNT"
trap - EXIT
echo "[5/5] flashing userdata (phone must be in TWRP)"
python3 tools/flash_userdata.py "$DST"
adb shell "printf 'boot-recovery\0' | dd of=/dev/block/bootdevice/by-name/misc bs=14 count=1 conv=notrunc" 2>/dev/null
echo "rootfs ready. Now: bash tools/round.sh"
