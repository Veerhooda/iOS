#!/bin/bash
# Post-run pipeline: pull ramoops + newest tracev3 files, mine First-throw stacks.
# Usage: postrun.sh <run#>   (device must be in recovery/TWRP with adb)
RUN="$1"
cd "$(dirname "$0")/.." || exit 1
echo "== [run$RUN] pulling ramoops"
adb pull /sys/fs/pstore/console-ramoops-0 logs/run${RUN}_ramoops.txt 2>&1 | tail -n 1
echo "== [run$RUN] crash census"
for p in SpringBoard suggestd itunescloudd ndoagent; do
  n=$(grep -c "OP2 EXIT #[0-9]*: pid=[0-9]*\[$p\] sig=6" logs/run${RUN}_ramoops.txt)
  echo "  $p sig6: $n"
done
echo "== [run$RUN] slide"
python3 tools/op2slide.py logs/run${RUN}_ramoops.txt | tail -n 1
echo "== [run$RUN] pulling newest tracev3 (by number, clock is broken)"
mkdir -p /tmp/postrun$RUN
# list Persist files, take highest-numbered 6 (freshest), pull each via dd+base64? No:
# use adb pull on the raw partition? Too big. Instead: TWRP can't read HFSX.
# Fallback: full capped pull like live6g (6GB). Do it in background outside this script.
echo "  run: adb exec-out 'dd if=/dev/block/bootdevice/by-name/userdata bs=1M count=6144 2>/dev/null | gzip -1 -c' > /tmp/postrun${RUN}.gz"
