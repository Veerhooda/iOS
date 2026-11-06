#!/bin/bash
# Mine fresh tracev3 + system.log from a mounted live userdata image.
# Usage: mine_fresh.sh <mountpoint> <outdir>
MNT="$1"; OUT="$2"
mkdir -p "$OUT"
echo "== newest tracev3 =="
ls -lt "$MNT/private/var/db/diagnostics/" 2>/dev/null | head -n 8
ls -lt "$MNT/private/var/db/uuidtext/" 2>/dev/null | head -n 4
echo "== system.log tail (termination reasons) =="
LOG=$(ls -t "$MNT/private/var/log/system.log"* 2>/dev/null | head -n 1)
echo "log=$LOG"
grep -a "Terminating app" "$LOG" 2>/dev/null | tail -n 30 > "$OUT/terminations.txt"
wc -l "$OUT/terminations.txt"
echo "== First-throw stacks with wide context =="
LATEST=$(ls -t "$MNT/private/var/db/diagnostics/"*.tracev3 2>/dev/null | head -n 2)
for f in $LATEST; do
  echo "-- $f"
  strings -n 8 "$f" | grep -A 16 "First throw call stack" > "$OUT/stacks_$(basename $f).txt"
done
wc -l "$OUT"/stacks_*.txt 2>/dev/null
echo "== CrashReporter =="
ls -lt "$MNT/private/var/mobile/Library/Logs/CrashReporter/" 2>/dev/null | head -n 20
