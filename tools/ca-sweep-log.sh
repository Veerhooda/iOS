#!/usr/bin/env bash
# Host sweep (docs/FABLE-REVIEW.md C1): every function-constant set CoreAnimation asked the phone for, taken from
# OP2Metal's pipeline lines in kernel logs ("M backboardd: P<n> ok|FAILED ... fc 0=..,11=.."), compiled for the A430
# on the Mac. Says for each stage how many compile, how many read the framebuffer (isam*), and prints every error.
# No boot is worth making while this reports an error.
#   bash tools/ca-sweep-log.sh [LOG ...]      (default: the two newest logs/daily-*.log)
set -uo pipefail
cd "$(dirname "$0")/.."
I=out/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler
M=research/ios12-metallib/QuartzCore.default.metallib
[ -x "$I" ] && [ -f "$M" ] || { echo "need $I (tools/mesa-host.sh) and $M" >&2; exit 2; }
[ $# -gt 0 ] || set -- $(ls -t logs/daily-*.log | head -2)
OUT=logs/ca-sweep-log; rm -rf "$OUT"; mkdir -p "$OUT"
grep -hE "M [A-Za-z]+: P[0-9]+ (ok|FAILED)" "$@" | sed -E 's/.* fc ([0-9a-f=,]+).*/\1/' | grep -E '^0=' | sort -u > "$OUT/fc.txt"
total=$(wc -l < "$OUT/fc.txt" | tr -d ' ')
echo "$total distinct constant sets in: $*"
rc=0
for fn in ca_uber_vertex_lp0 ca_uber_fragment_lp0_cp0_fo0; do
	ok=0; fail=0; fetch=0; n=0
	while read -r fc; do
		n=$((n + 1))
		f="$OUT/$fn-$n.txt"
		env "AIR_FC=$(echo "$fc" | sed -E 's/=([0-9a-f]+)/=0x\1/g')" perl -e 'alarm 60; exec @ARGV' "$I" -g 430 "$M" "$fn" > "$f" 2>&1
		if [ $? -eq 0 ] && grep -qE '[0-9]+ instr' "$f"; then
			ok=$((ok + 1))
			grep -qE '\bisam' "$f" && fetch=$((fetch + 1))
		else
			fail=$((fail + 1)); rc=1
			echo "  FAIL $fn fc $fc: $(grep -aE 'Unhandled|failed assert|Assertion|air:' "$f" | grep -v '^MESA: error: *$' | head -1 | cut -c1-140)"
		fi
	done < "$OUT/fc.txt"
	echo "$fn: $ok compile, $fail fail, $fetch read the framebuffer"
done
exit $rc
