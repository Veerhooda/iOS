#!/usr/bin/env bash
# G2 sweep: every function of a .metallib through AIR -> NIR -> ir3 for one Adreno (default 430 = the OnePlus 2),
# each in its own process (an assertion is a line, not the end of the sweep). Host Mesa from tools/mesa-host.sh.
#   tools/ca-sweep.sh [METALLIB] [GPU]      -> logs/ca-sweep-<gpu>.txt (+ per-function output in logs/ca-sweep-<gpu>/)
set -uo pipefail
cd "$(dirname "$0")/.."
LIB=${1:-research/ios12-metallib/QuartzCore.default.metallib}
GPU=${2:-430}
B=out/mesa-host/build
AIR2NIR=$B/src/compiler/air/air2nir
IR3=$B/src/gallium/drivers/freedreno/ir3_compiler
OUT=logs/ca-sweep-$GPU
mkdir -p "$OUT"
REPORT=logs/ca-sweep-$GPU.txt
: > "$REPORT"
ok=0; fail=0
while read -r name kind _; do
	[ -n "$name" ] || continue
	f="$OUT/$name.txt"
	# 60 s per function (perl alarm: macOS has no timeout(1))
	perl -e 'alarm 60; exec @ARGV' "$IR3" -g "$GPU" "$LIB" "$name" > "$f" 2>&1
	rc=$?
	instrs=$(grep -m1 -oE '[0-9]+ instr' "$f" | head -1)
	[ -n "$instrs" ] || instrs=$(grep -cE '^ *[0-9]+\[[0-9a-fx]+\]|^\s+(mov|add|mad|sam|ldg|stg|cov|end)' "$f")
	if [ $rc -eq 0 ] && grep -q "end" "$f"; then
		ok=$((ok+1)); printf "ok    %-44s %-9s %s\n" "$name" "$kind" "$instrs" >> "$REPORT"
	else
		fail=$((fail+1))
		why=$(grep -m1 -iE "error|assert|unsupported|unknown|abort|fail|not" "$f" | cut -c1-110)
		[ $rc -ge 128 ] && why="signal $((rc-128)) ${why}"
		printf "FAIL  %-44s %-9s rc=%d %s\n" "$name" "$kind" "$rc" "$why" >> "$REPORT"
	fi
done < <("$AIR2NIR" --list "$LIB")
echo "$(basename "$LIB") for a$GPU: $ok compiled, $fail failed (of $((ok+fail)))" | tee -a "$REPORT"
