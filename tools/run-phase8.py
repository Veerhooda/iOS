#!/usr/bin/env python3
"""
tools/run-phase8.py: Automated physical hardware boot and diagnostic harness
for Phase 8 (Hardware MDP5 Pipe Direct Scanout & 32bpp Framebuffer Carveout).
"""
import subprocess
import time
import os
import sys

SERIAL = "ebd7ee57"
BOOT_IMG = "out/xnu-boot.img"
RECOVERY_IMG = "backup/recovery.img"
RAMOOPS_LOG = "logs/hardware-phase8-mdp5-direct-scanout-ramoops.log"
TIMEOUT_SEC = 850  # 14 minutes (deadline is 600s = 10 min)

print("[PHASE 8 RUNNER] Starting Phase 8 Hardware MDP5 Pipe Direct Scanout Boot Run...", flush=True)

# Step 0: Ensure BCB boot-recovery is set
print("[PHASE 8 RUNNER] Step 0: Ensuring BCB boot-recovery is armed...", flush=True)
try:
    subprocess.run(["adb", "-s", SERIAL, "shell", "printf 'boot-recovery\\0' | dd of=/dev/block/bootdevice/by-name/misc bs=14 count=1 conv=notrunc"], timeout=5)
except Exception as e:
    print(f"[PHASE 8 RUNNER] Warning: Failed to set BCB via adb: {e}", flush=True)

# Step 1: Check device state
print("[PHASE 8 RUNNER] Step 1: Checking device state...", flush=True)
adb_res = subprocess.run(["adb", "devices"], capture_output=True, text=True)
print(adb_res.stdout, flush=True)

if SERIAL in adb_res.stdout:
    print("[PHASE 8 RUNNER] Device detected in ADB. Rebooting to bootloader...", flush=True)
    subprocess.run(["adb", "-s", SERIAL, "reboot", "bootloader"], check=True)
    time.sleep(2)

# Step 2: Wait for Fastboot
print("[PHASE 8 RUNNER] Step 2: Waiting for Fastboot...", flush=True)
fb_ready = False
for i in range(25):
    try:
        fb = subprocess.run(["fastboot", "devices"], capture_output=True, text=True, timeout=2.0)
        if SERIAL in fb.stdout:
            print(f"[PHASE 8 RUNNER] Fastboot ready (iteration {i+1})", flush=True)
            fb_ready = True
            break
    except Exception:
        pass
    time.sleep(1)

if not fb_ready:
    print("[PHASE 8 RUNNER] ERROR: Fastboot not detected!", flush=True)
    sys.exit(1)

# Step 3: Fastboot boot
print(f"[PHASE 8 RUNNER] Step 3: Booting XNU ({BOOT_IMG})...", flush=True)
boot_res = subprocess.run(["fastboot", "-s", SERIAL, "boot", BOOT_IMG], capture_output=True, text=True)
print(boot_res.stdout, boot_res.stderr, flush=True)
if boot_res.returncode != 0:
    print("[PHASE 8 RUNNER] ERROR: Boot command failed!", flush=True)
    sys.exit(1)

# Step 4: Monitor execution (deadline is 600s, poll up to TIMEOUT_SEC)
print(f"[PHASE 8 RUNNER] Step 4: XNU booted! Monitoring for reset (deadline = 1800s, max {TIMEOUT_SEC}s)...", flush=True)
start_time = time.time()
while time.time() - start_time < TIMEOUT_SEC:
    elapsed = int(time.time() - start_time)

    # Check for Fastboot reboot
    try:
        fb = subprocess.run(["fastboot", "devices"], capture_output=True, text=True, timeout=1.0)
        if SERIAL in fb.stdout:
            print(f"\n[PHASE 8 RUNNER] Fastboot detected at {elapsed}s! Booting recovery to harvest pstore...", flush=True)
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
                print(f"\n[PHASE 8 RUNNER] ADB recovery detected at {elapsed}s!", flush=True)
                break
        else:
            if elapsed % 30 == 0:
                print(f"[PHASE 8 RUNNER] Still executing XNU... ({elapsed}s / {TIMEOUT_SEC}s elapsed)", flush=True)
            time.sleep(1)
            continue
        break
    except Exception:
        pass

    time.sleep(1)

# Step 5: Wait for ADB recovery to settle
print("[PHASE 8 RUNNER] Step 5: Waiting for ADB recovery to settle...", flush=True)
adb_settled = False
for i in range(30):
    adb = subprocess.run(["adb", "devices"], capture_output=True, text=True)
    if SERIAL in adb.stdout and "recovery" in adb.stdout:
        print(f"[PHASE 8 RUNNER] ADB recovery ready (iteration {i+1})!", flush=True)
        adb_settled = True
        break
    time.sleep(2)

if not adb_settled:
    print("[PHASE 8 RUNNER] ERROR: Device did not return to ADB recovery within timeout!", flush=True)
    sys.exit(1)

# Step 6: Harvest pstore ramoops
print(f"[PHASE 8 RUNNER] Step 6: Harvesting /sys/fs/pstore/console-ramoops-0 to {RAMOOPS_LOG}...", flush=True)
time.sleep(2)
pull = subprocess.run(["adb", "-s", SERIAL, "pull", "/sys/fs/pstore/console-ramoops-0", RAMOOPS_LOG], capture_output=True, text=True)
print(pull.stdout, pull.stderr, flush=True)

if os.path.exists(RAMOOPS_LOG):
    size = os.path.getsize(RAMOOPS_LOG)
    print(f"[PHASE 8 RUNNER] Success! Harvested {size} bytes to {RAMOOPS_LOG}.", flush=True)
    # Re-arm BCB cookie
    print("[PHASE 8 RUNNER] Re-arming BCB boot-recovery in misc partition...", flush=True)
    subprocess.run(["adb", "-s", SERIAL, "shell", "printf 'boot-recovery\\0' | dd of=/dev/block/bootdevice/by-name/misc bs=14 count=1 conv=notrunc"], timeout=5)
else:
    print(f"[PHASE 8 RUNNER] ERROR: {RAMOOPS_LOG} not created!", flush=True)
    sys.exit(1)

print("[PHASE 8 RUNNER] COMPLETED_RUN", flush=True)
