#!/usr/bin/env bash
# Phone must be in TWRP (adb). Re-arms the BCB recovery cookie, goes to
# fastboot and RAM-boots out/xnu-boot.img. Never flashes anything.
set -euo pipefail
cd "$(dirname "$0")/.."
SERIAL="${SERIAL:-ebd7ee57}"
adb -s "$SERIAL" shell "printf 'boot-recovery\0' | dd of=/dev/block/bootdevice/by-name/misc bs=14 count=1 conv=notrunc" 2>/dev/null
echo -n "BCB cookie: "; adb -s "$SERIAL" shell "dd if=/dev/block/bootdevice/by-name/misc bs=1 count=13 2>/dev/null"; echo
adb -s "$SERIAL" reboot bootloader
for i in $(seq 1 30); do fastboot devices | grep -q "$SERIAL" && break; sleep 1; done
fastboot -s "$SERIAL" boot out/xnu-boot.img
echo "Booted. When done (or after a crash/reset), run ./tools/pull-log.sh"
