#!/usr/bin/env bash
# While a round runs: report when the phone's USB console appears (ioreg) and record its tty to logs/usb-console.log.
cd "$(dirname "$0")/.."
OUT=logs/usb-console.log; : > "$OUT"
seen=0
for i in $(seq 1 ${1:-600}); do
  if [ $seen = 0 ] && ioreg -p IOUSB -l -w0 2>/dev/null | grep -q '"OP2 USB console"\|"OnePlus 2 XNU"'; then
    seen=1; echo "$(date +%T) USB device seen:" | tee -a logs/usb-watch.txt
    ioreg -p IOUSB -l -w0 | grep -B2 -A20 '"OP2 USB console"' | grep -E 'USB Product|idVendor|idProduct|Speed|locationID' | head | tee -a logs/usb-watch.txt
  fi
  dev=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  if [ -n "$dev" ]; then
    echo "$(date +%T) tty $dev: recording" | tee -a logs/usb-watch.txt
    stty -f "$dev" 115200 raw 2>/dev/null
    cat "$dev" >> "$OUT"
    echo "$(date +%T) tty closed" | tee -a logs/usb-watch.txt
  fi
  grep -q "^DONE\|FAILED" logs/ROUND_STATUS.txt 2>/dev/null && [ $i -gt 30 ] && break
  sleep 1
done
