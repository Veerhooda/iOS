#!/usr/bin/env bash
# The frames OP2Metal captured in backboardd (docs/FABLE-REVIEW.md C2): /private/var/tmp/op2cap-<n>.bgra on the
# phone's rootfs -> logs/cap/<tag>-<n>-c<commit>.png. Phone in TWRP.
#   bash tools/op2cap-pull.sh [tag]        (tag: default the newest daily log's time)
set -uo pipefail
cd "$(dirname "$0")/.."
TAG=${1:-$(ls -t logs/daily-*.log | head -1 | sed -E 's/.*daily-(.*)\.log/\1/')}
mkdir -p logs/cap
for n in 0 1 2 3 4 5 6 7; do
	raw=logs/cap/$TAG-$n.bgra
	python3 tools/op2hfs.py read /private/var/tmp/op2cap-$n.bgra 0 16 "$raw.hdr" > /dev/null 2>&1 || continue
	read -r magic w h commit <<< "$(python3 -c "
import struct,sys
m,w,h,c=struct.unpack('<4I',open(sys.argv[1],'rb').read(16)); print('%08x'%m,w,h,c)" "$raw.hdr")"
	rm -f "$raw.hdr"
	[ "$magic" = "4332504f" ] || { echo "op2cap-$n: no capture"; continue; }
	python3 tools/op2hfs.py read /private/var/tmp/op2cap-$n.bgra 16 $((w * h * 4)) "$raw" > /dev/null || continue
	png=logs/cap/$TAG-$n-c$commit.png
	ffmpeg -v error -y -f rawvideo -pixel_format bgra -video_size ${w}x${h} -i "$raw" "$png" && rm -f "$raw"
	echo "$png (${w}x${h}, commit $commit)"
done
