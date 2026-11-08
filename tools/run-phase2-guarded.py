#!/usr/bin/env python3
import subprocess
import time
import sys

SERIAL = "ebd7ee57"
BOOT_IMG = "out/xnu-boot.img"
RECOVERY_IMG = "backup/recovery.img"
RAMOOPS_LOG = "logs/hardware-phase2-guarded-ramoops.log"

print("[RUNNER] Step 1: Checking current device state...", flush=True)
adb_res = subprocess.run(["adb", "devices"], capture_output=True, text=True)
print(adb_res.stdout, flush=True)

if SERIAL in adb_res.stdout:
    print("[RUNNER] Device in ADB. Rebooting to bootloader...", flush=True)
    subprocess.run(["adb", "-s", SERIAL, "reboot", "bootloader"], check=True)
    time.sleep(2)

print("[RUNNER] Step 2: Waiting for Fastboot...", flush=True)
for i in range(20):
    try:
        fb = subprocess.run(["fastboot", "-s", SERIAL, "getvar", "product"], capture_output=True, text=True, timeout=1)
        if "product:" in fb.stderr or "product:" in fb.stdout or fb.returncode == 0:
            print(f"[RUNNER] Fastboot ready (iteration {i+1})", flush=True)
            break
    except Exception:
        pass
    time.sleep(1)

print(f"[RUNNER] Step 3: Booting XNU ({BOOT_IMG})...", flush=True)
boot_res = subprocess.run(["fastboot", "-s", SERIAL, "boot", BOOT_IMG], capture_output=True, text=True)
print(boot_res.stdout, boot_res.stderr, flush=True)
if boot_res.returncode != 0:
    print("[RUNNER] Boot failed!", flush=True)
    sys.exit(1)

print("[RUNNER] Step 4: XNU booted! Monitoring for reset (deadline = 60s)...", flush=True)
start_time = time.time()
while time.time() - start_time < 200:
    elapsed = int(time.time() - start_time)

    # 1. Check if device rebooted into fastboot
    try:
        fb = subprocess.run(["fastboot", "-s", SERIAL, "getvar", "product"], capture_output=True, text=True, timeout=0.8)
        if "product:" in fb.stderr or "product:" in fb.stdout or fb.returncode == 0:
            print(f"\n[RUNNER] Fastboot detected at {elapsed}s! Booting recovery to harvest pstore...", flush=True)
            subprocess.run(["fastboot", "-s", SERIAL, "boot", RECOVERY_IMG], capture_output=True, text=True)
            time.sleep(5)
            break
    except Exception:
        pass

    # 2. Check if device rebooted directly into ADB recovery
    try:
        adb = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=0.8)
        for line in adb.stdout.splitlines():
            if SERIAL in line and ("recovery" in line or "device" in line):
                print(f"\n[RUNNER] ADB recovery detected at {elapsed}s!", flush=True)
                break
        else:
            if elapsed % 10 == 0:
                print(f"[RUNNER] Still running... ({elapsed}s / 60s)", flush=True)
            time.sleep(1)
            continue
        break
    except Exception:
        pass

    time.sleep(1)

print("[RUNNER] Step 5: Waiting for ADB in recovery to settle...", flush=True)
for i in range(25):
    adb = subprocess.run(["adb", "devices"], capture_output=True, text=True)
    if SERIAL in adb.stdout and "recovery" in adb.stdout:
        print(f"[RUNNER] ADB recovery ready (iteration {i+1})!", flush=True)
        break
    time.sleep(2)

print("[RUNNER] Step 6: Pulling /sys/fs/pstore/console-ramoops-0...", flush=True)
time.sleep(2)
pull = subprocess.run(["adb", "-s", SERIAL, "pull", "/sys/fs/pstore/console-ramoops-0", RAMOOPS_LOG], capture_output=True, text=True)
print(pull.stdout, pull.stderr, flush=True)

subprocess.run(["ls", "-la", RAMOOPS_LOG])
print("[RUNNER] FINISHED_HARVEST", flush=True)
