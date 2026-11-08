#!/usr/bin/env python3
"""
tools/run-phase3.py: Automated physical hardware boot and diagnostic harness
for Phase 3 (IOAccelerator User Client & Adreno 430 Hardware Dispatch).
"""
import subprocess
import time
import os
import sys

SERIAL = "ebd7ee57"
BOOT_IMG = "out/xnu-boot.img"
RECOVERY_IMG = "backup/recovery.img"
RAMOOPS_LOG = "logs/hardware-phase3-ramoops.log"

print("[PHASE 3 RUNNER] Starting Phase 3 Physical Hardware Boot Run...", flush=True)

# Step 1: Check device state
print("[PHASE 3 RUNNER] Step 1: Checking device state...", flush=True)
adb_res = subprocess.run(["adb", "devices"], capture_output=True, text=True)
print(adb_res.stdout, flush=True)

if SERIAL in adb_res.stdout:
    print("[PHASE 3 RUNNER] Device detected in ADB. Rebooting to bootloader...", flush=True)
    subprocess.run(["adb", "-s", SERIAL, "reboot", "bootloader"], check=True)
    time.sleep(2)

# Step 2: Wait for Fastboot
print("[PHASE 3 RUNNER] Step 2: Waiting for Fastboot...", flush=True)
fb_ready = False
for i in range(25):
    try:
        fb = subprocess.run(["fastboot", "-s", SERIAL, "getvar", "product"], capture_output=True, text=True, timeout=1.5)
        if "product:" in fb.stderr or "product:" in fb.stdout or fb.returncode == 0:
            print(f"[PHASE 3 RUNNER] Fastboot ready (iteration {i+1})", flush=True)
            fb_ready = True
            break
    except Exception:
        pass
    time.sleep(1)

if not fb_ready:
    print("[PHASE 3 RUNNER] ERROR: Fastboot not detected!", flush=True)
    sys.exit(1)

# Step 3: Fastboot boot
print(f"[PHASE 3 RUNNER] Step 3: Booting XNU ({BOOT_IMG})...", flush=True)
boot_res = subprocess.run(["fastboot", "-s", SERIAL, "boot", BOOT_IMG], capture_output=True, text=True)
print(boot_res.stdout, boot_res.stderr, flush=True)
if boot_res.returncode != 0:
    print("[PHASE 3 RUNNER] ERROR: Boot command failed!", flush=True)
    sys.exit(1)

# Step 4: Monitor execution (deadline is 60s, poll up to 180s)
print("[PHASE 3 RUNNER] Step 4: XNU booted! Monitoring for reset (deadline = 60s)...", flush=True)
start_time = time.time()
while time.time() - start_time < 180:
    elapsed = int(time.time() - start_time)

    # Check for Fastboot reboot
    try:
        fb = subprocess.run(["fastboot", "-s", SERIAL, "getvar", "product"], capture_output=True, text=True, timeout=0.8)
        if "product:" in fb.stderr or "product:" in fb.stdout or fb.returncode == 0:
            print(f"\n[PHASE 3 RUNNER] Fastboot detected at {elapsed}s! Booting recovery to harvest pstore...", flush=True)
            subprocess.run(["fastboot", "-s", SERIAL, "boot", RECOVERY_IMG], capture_output=True, text=True)
            time.sleep(5)
            break
    except Exception:
        pass

    # Check for direct ADB recovery reboot
    try:
        adb = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=0.8)
        for line in adb.stdout.splitlines():
            if SERIAL in line and ("recovery" in line or "device" in line):
                print(f"\n[PHASE 3 RUNNER] ADB recovery detected at {elapsed}s!", flush=True)
                break
        else:
            if elapsed % 10 == 0:
                print(f"[PHASE 3 RUNNER] Still executing XNU... ({elapsed}s elapsed)", flush=True)
            time.sleep(1)
            continue
        break
    except Exception:
        pass

    time.sleep(1)

# Step 5: Wait for ADB recovery to settle
print("[PHASE 3 RUNNER] Step 5: Waiting for ADB recovery to settle...", flush=True)
adb_settled = False
for i in range(30):
    adb = subprocess.run(["adb", "devices"], capture_output=True, text=True)
    if SERIAL in adb.stdout and "recovery" in adb.stdout:
        print(f"[PHASE 3 RUNNER] ADB recovery ready (iteration {i+1})!", flush=True)
        adb_settled = True
        break
    time.sleep(2)

if not adb_settled:
    print("[PHASE 3 RUNNER] ERROR: Device did not return to ADB recovery within timeout!", flush=True)
    sys.exit(1)

# Step 6: Harvest pstore ramoops
print(f"[PHASE 3 RUNNER] Step 6: Harvesting /sys/fs/pstore/console-ramoops-0 to {RAMOOPS_LOG}...", flush=True)
time.sleep(2)
pull = subprocess.run(["adb", "-s", SERIAL, "pull", "/sys/fs/pstore/console-ramoops-0", RAMOOPS_LOG], capture_output=True, text=True)
print(pull.stdout, pull.stderr, flush=True)

if os.path.exists(RAMOOPS_LOG):
    size = os.path.getsize(RAMOOPS_LOG)
    print(f"[PHASE 3 RUNNER] Success! Harvested {size} bytes to {RAMOOPS_LOG}.", flush=True)
else:
    print(f"[PHASE 3 RUNNER] ERROR: {RAMOOPS_LOG} not created!", flush=True)
    sys.exit(1)

print("[PHASE 3 RUNNER] COMPLETED_RUN", flush=True)
