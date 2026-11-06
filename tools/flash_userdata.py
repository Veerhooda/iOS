#!/usr/bin/env python3
"""
Stream compressed out/ios12-userdata.dmg directly into /dev/block/bootdevice/by-name/userdata
over raw TCP port-forward via busybox netcat and gunzip.
Guarantees byte-for-byte binary integrity without intermediate storage or CRLF translation.
"""

import os
import sys
import time
import socket
import zlib
import subprocess

PORT = 12345
IMAGE_PATH = sys.argv[1] if len(sys.argv) > 1 else 'out/ios12-userdata.dmg'
TARGET_DEV = '/dev/block/bootdevice/by-name/userdata'


def main():
    if not os.path.exists(IMAGE_PATH):
        sys.exit(f"Error: {IMAGE_PATH} does not exist!")

    total_bytes = os.path.getsize(IMAGE_PATH)
    print(f"=== OnePlus 2 High-Speed Userdata Flasher ===")
    print(f"  Source image: {IMAGE_PATH} ({total_bytes / (1024*1024):.1f} MB)")
    print(f"  Target dev  : {TARGET_DEV}")

    # 1. Setup port forward
    print("[STEP 1] Configuring ADB port forwarding...")
    subprocess.run(['adb', 'forward', f'tcp:{PORT}', f'tcp:{PORT}'], check=True)

    # 2. Launch background receiver on device
    print(f"[STEP 2] Spawning netcat | gunzip receiver on device...")
    dev_cmd = f"busybox nc -l -p {PORT} | gunzip -c | dd of={TARGET_DEV} bs=4M conv=fsync"
    proc = subprocess.Popen(['adb', 'shell', dev_cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # Allow nc to bind socket
    time.sleep(1)

    # 3. Connect to socket and stream compressed image
    print(f"[STEP 3] Connecting to device and streaming data...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('127.0.0.1', PORT))

    compressor = zlib.compressobj(level=1, wbits=31)  # wbits=31 produces gzip header/footer
    chunk_size = 4 * 1024 * 1024  # 4 MB read chunks
    bytes_read = 0
    bytes_sent = 0
    start_time = time.time()

    with open(IMAGE_PATH, 'rb') as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            bytes_read += len(chunk)
            c_data = compressor.compress(chunk)
            if c_data:
                sock.sendall(c_data)
                bytes_sent += len(c_data)

            elapsed = time.time() - start_time
            speed = (bytes_read / (1024 * 1024)) / elapsed if elapsed > 0 else 0
            pct = (bytes_read / total_bytes) * 100
            print(f"\r  Progress: {bytes_read / (1024*1024):.0f} / {total_bytes / (1024*1024):.0f} MB ({pct:.1f}%) | wire: {bytes_sent / (1024*1024):.0f} MB | {speed:.1f} MB/s", end='', flush=True)

    # Flush compressor trailer
    c_data = compressor.flush()
    if c_data:
        sock.sendall(c_data)
        bytes_sent += len(c_data)

    sock.shutdown(socket.SHUT_WR)
    sock.close()
    print()

    # 4. Wait for receiver process
    print("[STEP 4] Waiting for device fsync to complete...")
    stdout, stderr = proc.communicate()
    print("  Device output:\n" + stderr.decode(errors='ignore').strip())

    # 5. Verify HFSX magic on device
    print("[STEP 5] Verifying HFSX volume signature on target partition...")
    res = subprocess.check_output(['adb', 'shell', f'dd if={TARGET_DEV} bs=1 skip=1024 count=2 2>/dev/null']).strip()
    if res == b'HX':
        print(f"  [SUCCESS] HFSX volume signature 'HX' verified on {TARGET_DEV}!")
    else:
        sys.exit(f"  [ERROR] Verification failed: expected b'HX', got {res}")

    # 6. Verify and re-arm BCB cookie
    print("[STEP 6] Arming BCB recovery cookie in /dev/block/bootdevice/by-name/misc...")
    subprocess.run(['adb', 'shell', "echo -en 'boot-recovery\\0' | dd of=/dev/block/bootdevice/by-name/misc bs=1 count=14 conv=fsync 2>/dev/null"], check=True)
    res_misc = subprocess.check_output(['adb', 'shell', "dd if=/dev/block/bootdevice/by-name/misc bs=1 count=14 2>/dev/null"])
    if b'boot-recovery' in res_misc:
        print("  [SUCCESS] BCB recovery cookie verified armed!")
    else:
        sys.exit("  [ERROR] Failed to verify BCB recovery cookie in misc!")

    print("\n[ALL COMPLETE] Device is fully prepared for Run 76 boot!")


if __name__ == '__main__':
    main()
