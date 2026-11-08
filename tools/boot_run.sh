#!/usr/bin/env bash
#
# Automated Boot & Diagnostic Tool for OnePlus 2 iOS Bring-Up.
# Reboots device to bootloader, issues fastboot boot, and automatically
# collects ramoops kernel logs upon recovery reset.
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOOT_IMG="${1:-${ROOT_DIR}/out/xnu-boot.img}"
RUN_LOG_DIR="${ROOT_DIR}/logs"

mkdir -p "${RUN_LOG_DIR}"

if [ ! -f "${BOOT_IMG}" ]; then
    echo "[-] Error: Boot image not found: ${BOOT_IMG}" >&2
    exit 1
fi

echo "=== OnePlus 2 iOS Run Harness ==="
echo "  Boot Image: ${BOOT_IMG} ($(du -h "${BOOT_IMG}" | awk '{print $1}'))"

# Check device state
STATE=$(adb devices 2>/dev/null | grep -E "recovery|device" | awk '{print $2}' || true)
if [ "${STATE}" = "recovery" ] || [ "${STATE}" = "device" ]; then
    echo "[*] Rebooting from ${STATE} to bootloader..."
    adb reboot bootloader
    sleep 8
fi

echo "[*] Waiting for fastboot device..."
fastboot devices
echo "[+] Device detected in fastboot mode."

echo "[*] Booting ${BOOT_IMG}..."
fastboot boot "${BOOT_IMG}"

BOOT_TIME=$(date +"%Y-%m-%d %H:%M:%S")
echo "[+] Fastboot boot issued at: ${BOOT_TIME}"
echo "[*] Device is executing XNU. Waiting for automated deadline reset (~5-30 min)..."
echo "    (You can also trigger manual reset anytime by holding Power+VolDown)"

# Poll for device returning to TWRP recovery
while true; do
    sleep 10
    REC=$(adb devices 2>/dev/null | grep "recovery" || true)
    if [ -n "${REC}" ]; then
        echo "[+] Device returned to TWRP recovery!"
        break
    fi
done

# Wait for pstore mount
sleep 3
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
DEST_LOG="${RUN_LOG_DIR}/hardware-run-${TIMESTAMP}-ramoops.log"

echo "[*] Pulling /sys/fs/pstore/console-ramoops-0..."
adb pull /sys/fs/pstore/console-ramoops-0 "${DEST_LOG}"

echo "[+] Saved persistent log to: ${DEST_LOG}"
echo ""
echo "=== Execution Summary ==="
grep -E "OP2 FB|metal=|OP2Metal|IOAcceleratorES|AGXAccelerator|AppleCLCD|OP2 Buttons" "${DEST_LOG}" | head -n 30 || true
echo "========================="
