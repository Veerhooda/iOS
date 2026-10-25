# Android Partition Restore Guide (OnePlus 2)

This document provides instructions for restoring the factory Android partitions from local backups in [`backup/`](../backup/).

## 1. Prerequisites
- Device booted into TWRP Recovery (`adb devices` shows `... recovery`).
- Backup file integrity verified:
  ```bash
  gzip -t backup/system.img.gz
  ```

## 2. Restoring `system` Partition
The stock Android system partition (`/dev/block/bootdevice/by-name/system`) was backed up to `backup/system.img.gz` (size 1.2 GB compressed, 2.5 GiB raw).

To restore it directly over ADB streaming:
```bash
gzip -dc backup/system.img.gz | adb shell "dd of=/dev/block/bootdevice/by-name/system bs=4M"
```

After flashing completes, verify partition readback:
```bash
adb shell "sync"
```

## 3. Restoring Other Critical Partitions
All 40 partitions dumped during Milestone 0 are located in [`backup/`](../backup/) with checksums listed in [`backup/SHA256SUMS`](../backup/SHA256SUMS):
- `boot.img` -> `/dev/block/bootdevice/by-name/boot`
- `recovery.img` -> `/dev/block/bootdevice/by-name/recovery`
- `aboot.img` -> `/dev/block/bootdevice/by-name/aboot`

Example to restore boot:
```bash
adb shell "dd if=/dev/zero of=/dev/block/bootdevice/by-name/boot bs=4M" # optional wipe
adb push backup/boot.img /tmp/boot.img
adb shell "dd if=/tmp/boot.img of=/dev/block/bootdevice/by-name/boot bs=4M && rm /tmp/boot.img"
```
