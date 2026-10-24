#!/usr/bin/env python3
import subprocess, os, sys, hashlib

PARTITIONS = [
    'DDR', 'DRIVER', 'LOGO', 'aboot', 'abootbak', 'apdp', 'bluetooth',
    'boot', 'config', 'devinfo', 'dpo', 'fsc', 'fsg', 'hyp', 'hypbak',
    'keystore', 'limits', 'misc', 'modem', 'modemst1', 'modemst2', 'msadp',
    'oem_dycnvbk', 'oem_stanvbk', 'param', 'persist', 'pmic', 'pmicbak',
    'recovery', 'reserve1', 'reserve2', 'rpm', 'rpmbak', 'sbl1', 'sbl1bak',
    'sdi', 'sec', 'ssd', 'tz', 'tzbak'
]

os.makedirs('backup', exist_ok=True)
sums = []

print(f"Starting backup of {len(PARTITIONS)} partitions...")

for part in PARTITIONS:
    src = f"/dev/block/bootdevice/by-name/{part}"
    dst = f"backup/{part}.img"
    
    # 1. Device SHA256
    dev_sha_proc = subprocess.run(['adb', 'shell', f'sha256sum {src}'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    dev_sha = dev_sha_proc.stdout.strip().split()[0]
    
    # 2. Dump partition
    with open(dst, 'wb') as f:
        subprocess.run(['adb', 'exec-out', f'cat {src}'], stdout=f, check=True)
        
    # 3. Host SHA256
    h = hashlib.sha256()
    with open(dst, 'rb') as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    host_sha = h.hexdigest()
    
    if dev_sha != host_sha:
        print(f"ERROR: Hash mismatch for {part}! Dev: {dev_sha} != Host: {host_sha}")
        sys.exit(1)
        
    print(f"OK: {part:12s} ({os.path.getsize(dst):10d} bytes) -> {host_sha[:16]}...")
    sums.append(f"{host_sha}  {part}.img")

with open('backup/SHA256SUMS', 'w') as f:
    f.write('\n'.join(sums) + '\n')

print("Backup complete and verified successfully!")
