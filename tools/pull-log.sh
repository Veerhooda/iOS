#!/usr/bin/env bash
# After the phone is back in TWRP: save the previous boot's kernel log and
# print a short health summary (CPU clock, display perf, crashes, panics).
set -euo pipefail
cd "$(dirname "$0")/.."
SERIAL="${SERIAL:-ebd7ee57}"
OUT="logs/daily-$(date +%Y%m%d-%H%M%S).log"
adb -s "$SERIAL" wait-for-recovery 2>/dev/null || adb -s "$SERIAL" wait-for-device
adb -s "$SERIAL" pull /sys/fs/pstore/console-ramoops-0 "$OUT" >/dev/null
adb -s "$SERIAL" shell "printf 'boot-recovery\0' | dd of=/dev/block/bootdevice/by-name/misc bs=14 count=1 conv=notrunc" 2>/dev/null
echo "Saved $OUT"
echo "--- summary ---"
grep -aE "effective clock|OP2 BOOST|OP2 TOP|eMMC MULTI|multi-block|Home key|double buffering|SwapWait\(|OP2 FB PERF|synthetic input|panic\(|SIGSEGV|SIGABRT|EXC_|exited|jetsam|burning CPU|wake-only" "$OUT" | cut -c1-200 | tail -40 || true
