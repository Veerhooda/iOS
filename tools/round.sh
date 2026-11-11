#!/usr/bin/env bash
# One full test round, hands-off: build -> RAM-boot -> wait for the phone to
# come back to TWRP (crash or deadline) -> save log. Everything lands in
# logs/ so Claude can read it from the shared folder.
#   ./tools/round.sh                # 15 min run
#   DEADLINE=1800 ./tools/round.sh  # 30 min run
set -uo pipefail
cd "$(dirname "$0")/.."
export DEADLINE="${DEADLINE:-900}"
STATUS=logs/ROUND_STATUS.txt
echo "building $(date)" > "$STATUS"
if ! bash ./tools/build-daily.sh "$@" > logs/last-build.txt 2>&1; then
  grep -nE "error:|Error|FAILED" logs/last-build.txt build.log 2>/dev/null | head -40 > logs/last-build-errors.txt
  echo "BUILD FAILED $(date) - see logs/last-build-errors.txt" > "$STATUS"; cat "$STATUS"; exit 1
fi
echo "booting $(date)" > "$STATUS"
bash ./tools/boot-daily.sh || { echo "BOOT FAILED $(date)" > "$STATUS"; exit 1; }
echo "running (deadline ${DEADLINE}s) since $(date)" > "$STATUS"
sleep 60
for i in $(seq 1 $(( (DEADLINE + 600) / 10 ))); do
  adb devices | grep -q "recovery" && break
  sleep 10
done
sleep 5
bash ./tools/pull-log.sh > logs/last-summary.txt 2>&1
echo "DONE $(date) - $(head -1 logs/last-summary.txt)" > "$STATUS"
cat logs/last-summary.txt
