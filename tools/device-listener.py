#!/usr/bin/env python3
import subprocess
import time
import sys

SERIAL = "ebd7ee57"
RECOVERY_IMG = "backup/recovery.img"

print(f"[LISTENER] Watching for OnePlus 2 ({SERIAL}) in fastboot or recovery mode...", flush=True)

while True:
    # 1. Check fastboot
    try:
        fb = subprocess.run(
            ["fastboot", "-s", SERIAL, "getvar", "product"],
            capture_output=True,
            text=True,
            timeout=1
        )
        if "product:" in fb.stderr or "product:" in fb.stdout or fb.returncode == 0:
            print(f"\n[LISTENER] FASTBOOT DETECTED! Booting TWRP recovery via {RECOVERY_IMG}...", flush=True)
            res = subprocess.run(
                ["fastboot", "-s", SERIAL, "boot", RECOVERY_IMG],
                capture_output=True,
                text=True
            )
            print("[LISTENER] Fastboot boot output:", res.stdout, res.stderr, flush=True)
            print("[LISTENER] SUCCESS_BOOTED_RECOVERY", flush=True)
            break
    except subprocess.TimeoutExpired:
        pass
    except Exception as e:
        pass

    # 2. Check adb
    try:
        adb = subprocess.run(
            ["adb", "devices"],
            capture_output=True,
            text=True,
            timeout=1
        )
        for line in adb.stdout.splitlines():
            if SERIAL in line and ("recovery" in line or "device" in line):
                print(f"\n[LISTENER] ADB DETECTED! Device is in state: {line}", flush=True)
                print("[LISTENER] SUCCESS_ADB_RECOVERY", flush=True)
                sys.exit(0)
    except Exception:
        pass

    time.sleep(0.5)
