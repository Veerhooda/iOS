#!/usr/bin/env bash
# The CoreAnimation pipelines as the phone really asked for them: function constants captured by OP2Metal
# (boot with EXTRA="metal=2", then tools/fc-from-log.py LOG > logs/ca-fc-captured.txt), each compiled for the A430.
#   tools/ca-compile-captured.sh [CAPTURED] [GPU]
set -uo pipefail
cd "$(dirname "$0")/.."
CAP=${1:-logs/ca-fc-captured.txt}
GPU=${2:-430}
I=out/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler
M=research/ios12-metallib/QuartzCore.default.metallib
OUT=logs/ca-sweep-$GPU-real
mkdir -p "$OUT"
ok=0; fail=0
while read -r prog fn fc; do
	f="$OUT/$prog-$fn.txt"
	env "$fc" perl -e 'alarm 60; exec @ARGV' "$I" -g "$GPU" "$M" "$fn" > "$f" 2>&1
	rc=$?
	n=$(grep -m1 -oE '[0-9]+ instr' "$f")
	if [ $rc -eq 0 ]; then ok=$((ok+1)); echo "ok    $prog $fn $n"; else fail=$((fail+1)); echo "FAIL  $prog $fn rc=$rc $(grep -a -v '^ \|AIR_FC' "$f" | tail -1 | cut -c1-150)"; fi
done < "$CAP"
echo "captured pipelines for a$GPU: $ok compiled, $fail failed"
