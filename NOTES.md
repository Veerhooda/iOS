# iOS on OnePlus 2 (Snapdragon 810 / MSM8994) - Engineering Notes

## Project Overview
Porting Apple's XNU kernel and iOS userland components to the OnePlus 2 (`oneplus2` / `ONE A2003`, Qualcomm Snapdragon 810 / MSM8994), inspired by [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note 10 Pro / SM7150).

---

## Hardware Facts & System Architecture (Verified from Hardware)

### 1. Memory Map & Usable RAM Ranges
Extracted from live `/proc/iomem` and decompiled `live.dts` (`MSM8994v2.1 MTP`):
* **Lower RAM Bank**: `0x00000000 - 0x062fffff` (~99 MB)
  * Linux Kernel loaded at: `0x00080000`
  * Kernel Data: `0x01445000 - 0x018999f7`
* **Qualcomm Protected Carve-Outs (Removed from OS)**:
  * `0x06300000 - 0x07000000`: `memory_hole` (13 MB)
  * `0x07000000 - 0x0ca00000`: `modem_mem` (90 MB)
  * `0x0ca00000 - 0x0e900000`: `peripheral_mem` (31 MB)
  * `0x0e900000 - 0x10200000`: `tzapp_mem` (25 MB)
* **Main Usable System RAM**:
  * **`0x10200000 - 0xf7ffffff`**: **~3.7 GB contiguous RAM bank** (completely free of carve-outs except minor diagnostic regions).
  * Minor reservations in upper RAM:
    * `0xac000000 - 0xac100000`: `ramoops_mem` (1 MB)
    * `0xac200000 - 0xac300000`: `param_mem` (1 MB)
    * `0xac400000 - 0xac500000`: `memory_mtp` (1 MB)

### 2. Display & Continuous Framebuffer
Queried via `/sys/class/graphics/fb0/`, dmesg, and `live.dts`:
* **Driver**: Qualcomm Mobile Display Subsystem (`mdssfb_90000`, `fd998000.qcom,mdss_dsi`)
* **Continuous Splash Physical Base**: **`0x03401000`** (node: `cont_splash_mem@0`, size: `0x2200000` = 34 MB)
* **Panel Resolution**: **1080 × 1920** (16:9 aspect ratio)
* **Virtual Size**: `1080, 3840` (double-buffered)
* **Pixel Format**: 32-bit ARGB/RGBA (`bits_per_pixel = 32`)
* **Row Stride**: **`4352` bytes** (`1080 * 4 = 4320`, padded to 4352 bytes / 1088 words for 128-byte hardware alignment)

### 3. Interrupt Controller (GICv2)
Confirmed via `interrupt-controller@f9000000`:
* **Compatible**: `"qcom,msm-qgic2"` (ARM GICv2 compliant)
* **Distributor Interface (`GICD`)**: **`0xf9000000`**, size: `0x1000` (4 KB)
* **CPU Interface (`GICC`)**: **`0xf9002000`**, size: `0x1000` (4 KB)
* **Significance**: Unlike `sweet`'s GICv3 (which uses system registers `ICC_*`), MSM8994 strictly requires memory-mapped MMIO register accesses to `0xf9002000` to acknowledge and end interrupts (`GICC_IAR`, `GICC_EOIR`, `GICC_PMR`).

### 4. CPU Topology & Bring-Up Method
Confirmed via `cpus` node in `live.dts`:
* **Cluster 0 (LITTLE)**:
  * `cpu@0`: ARM Cortex-A53 (MPIDR/reg `0x00`) - **Boot Core**
  * `cpu@1`: ARM Cortex-A53 (MPIDR/reg `0x01`)
  * `cpu@2`: ARM Cortex-A53 (MPIDR/reg `0x02`)
  * `cpu@3`: ARM Cortex-A53 (MPIDR/reg `0x03`)
* **Cluster 1 (big)**:
  * `cpu@4`: ARM Cortex-A57 (MPIDR/reg `0x100`)
  * `cpu@5`: ARM Cortex-A57 (MPIDR/reg `0x101`)
  * `cpu@6`: ARM Cortex-A57 (MPIDR/reg `0x102`)
  * `cpu@7`: ARM Cortex-A57 (MPIDR/reg `0x103`)
* **Enable Method**: `"qcom,8994-arm-cortex-acc"`
* **PSCI**: **None** (no PSCI node in DTB; secondary core boot uses Qualcomm ACC / APCS and SCM boot-address registers).

### 5. Architected Timer
Confirmed via `timer` node in `live.dts`:
* **Compatible**: `"arm,armv8-timer"`
* **Clock Frequency**: `0x124f800` = **19,200,000 Hz (19.2 MHz)**
* **PPI Interrupts**:
  * Secure Physical Timer: PPI 2 (INTID 18)
  * Non-Secure Physical Timer: PPI 3 (INTID 19)
  * Virtual Timer (CNTV): PPI 4 (INTID 20)
  * Hypervisor Timer: PPI 1 (INTID 17)

### 6. Storage Controller (eMMC 5.0)
Confirmed via `/sys/class/mmc_host/mmc0` and `sdhci@f9824900`:
* **Controller**: Qualcomm SDHCI MSM (`sdhc1`)
* **Compatible**: `"qcom,sdhci-msm"`
* **Host Controller Registers (`hc_mem`)**: **`0xf9824900`** (size: `0x1a0`)
* **Vendor Core Registers (`core_mem`)**: **`0xf9824000`** (size: `0x800`)
* **Interrupts**:
  * `hc_irq`: SPI 123 (INTID `32 + 123 = 155`)
  * `pwr_irq`: SPI 138 (INTID `32 + 138 = 170`)
* **Storage Medium**: 64 GB eMMC 5.0 (`mmcblk0`)

---

## Recovery & Emergency Download (EDL 9008) Reference

* **USB Identification**:
  * **EDL Mode**: `05c6:9008` (Qualcomm HS-USB QDLoader 9008)
  * **Fastboot Mode**: `18d1:d00d` or `2a70:9011`
  * **Android / ADB Mode**: `05c6:9025` or `05c6:9001` (`idVendor = 1478` decimal)
* **Entering EDL Mode on OnePlus 2**:
  1. Software trigger via ADB: `adb reboot edl`
  2. Software trigger via fastboot: `fastboot oem edl` or `fastboot reboot-edl`
  3. Hardware key combination: Power off completely, hold **Volume Up** while connecting the USB cable to PC.
  4. Hardware fallback: Deep Flash Cable (short D+ to GND with button) or motherboard test points under the back cover.
* **Unbrick Utility**: OnePlus 2 MSM Download Tool (uses Sahara / Firehose protocol `MPRG8994.mbn` + `prog_emmc_firehose_8994_lite.mbn` to reflash all partitions).

---

## Code Analysis: `ios-sweet` Components vs. OnePlus 2 Adaptations

### 1. `NOTES.md`
* `sweet`: Relied on 16 KB pages and 64-bit high memory bank (`0x100000000`).
* `oneplus2`: Strict 4 KB pages (`ARM_PGSHIFT = 12`), 32-bit address space `< 4 GB` using the main `0x10200000` bank.

### 2. `shim/` Directory
* **`start.S`**: Linux AArch64 Image header (`_head`) at `0x00080000`. Sets up stack, zeroes BSS, enters `shim_main(x0)` with FDT in `x0`.
  * *OP2 change*: Compatible as-is for LK bootloader; keep single-core `cpus=1` (omit PSCI secondaries).
* **`mmu.c`**: Configures EL1 Stage-1 MMU and page tables.
  * *OP2 change*: **Must rewrite from 16 KB to 4 KB translation granule**. L2 blocks are 2 MB (instead of 32 MB), L3 pages are 4 KB. Framebuffer `0x03401000` mapped as Normal Non-Cacheable; carveouts `0x06300000 - 0x10200000` unmapped.
* **`console.c`**: Pixel-font console rendered directly into continuous splash.
  * *OP2 change*: Set resolution to 1080×1920, stride to 4352 bytes, framebuffer base to `0x03401000`.
* **`loader.c`**: iBoot-style Mach-O segment loader and `boot_args` builder.
  * *OP2 change*: Point `physBase` to `0x10200000`, set `v_width=1080`, `v_height=1920`, `v_rowBytes=4352`, `v_baseAddr=0x03401000`.
* **`ufs.c`**: UFS host controller driver.
  * *OP2 change*: Disable entirely (OP2 uses eMMC 5.0, not UFS). Boot M1 without storage or via RAM disk.
* **`build.sh`**: Builds and packages boot image.
  * *OP2 change*: Use `clang -target aarch64-none-elf` and pack with `mkbootimg` header version 0.

### 3. `tools/mkadt.py`
* Emits binary Apple Device Tree (ADT).
  * *OP2 change*: Set `dram-base = 0x10200000`, `dram-size = 0xe7e00000` (~3.7 GB). Set GIC node to GICv2 MMIO distributor `0xf9000000` and CPU interface `0xf9002000`. Switch product node to iPhone 6 / 6 Plus identity.

---

## Partition Backup Log (Day 2)

All 40 partitions backed up from TWRP into `backup/` and verified with SHA-256 against device-computed hashes:

| Partition | Block Dev | Size (Bytes) | SHA-256 Digest (Prefix) |
| :--- | :--- | :--- | :--- |
| `boot` | `mmcblk0p35` | 67,108,864 | `28eb63da5aa56bc8...` |
| `recovery` | `mmcblk0p36` | 67,108,864 | `251efaf9cd6119dc...` |
| `persist` | `mmcblk0p37` | 33,554,432 | `73a53d784e251b67...` |
| `modemst1` | `mmcblk0p17` | 1,572,864 | `5d544ee320cf857f...` |
| `modemst2` | `mmcblk0p18` | 1,572,864 | `f7e10085e938169f...` |
| `fsg` | `mmcblk0p8` | 1,572,864 | `106f0647ae10a651...` |
| `fsc` | `mmcblk0p15` | 1,024 | `5f70bf18a0860070...` |
| `aboot` | `mmcblk0p10` | 1,048,576 | `aab4682faa93c487...` |
| `sbl1` | `mmcblk0p26` | 1,048,576 | `198be0270102b081...` |
| `tz` | `mmcblk0p29` | 1,048,576 | `002640e62ec2bbd1...` |
| `rpm` | `mmcblk0p31` | 512,000 | `bfec3e0b0ccf4f39...` |
| `DRIVER` | `mmcblk0p25` | 33,554,432 | `e557b79ed38d4052...` |
| `LOGO` | `mmcblk0p24` | 16,777,216 | `4388734fbed3a16d...` |
| `modem` | `mmcblk0p1` | 83,886,080 | `3d4b1c5f9b2ccff0...` |

Full checksums recorded in [`backup/SHA256SUMS`](backup/SHA256SUMS).
Second-pass checksum verification: **PASSED** on all critical partitions.

---

## Boot Image Header & Repack Verification (M0 Test)

Unpacked stock `backup/boot.img` with `tools/unpack_bootimg.py`:
* **Header Version**: `0`
* **Page Size**: `4096`
* **Base Address**: `0x00000000`
* **Kernel Load Address**: `0x00008000` (offset: `0x8000`)
* **Ramdisk Load Address**: `0x01000000` (offset: `0x1000000`)
* **Tags Load Address**: `0x00000100` (offset: `0x100`)
* **Command Line**:
  `androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x37 ehci-hcd.park=3 lpm_levels.sleep_disabled=1 boot_cpus=0-3 loop.max_part=7 androidboot.selinux=permissive buildvariant=userdebug`
* **Kernel & DTB**: `dtb/boot_unpacked/kernel` is `Image.gz-dtb` with 7 appended DTBs. DTB 0 corresponds to `Qualcomm Technologies, Inc. MSM8994v2.1 MTP` (287,284 bytes).
* **Repacked Test Image**: Generated [`out/repack-test.img`](out/repack-test.img). Unpacked and verified: kernel and ramdisk match byte-for-byte.
* **M0 Key Test (`fastboot boot out/repack-test.img`)**: **PASSED!**
  * Bootloader loaded image via fastboot (`OKAY [0.269s]`, Booting `OKAY [0.628s]`).
  * Android 10 booted successfully to the home screen.
  * Verified via adb: system online (`uptime: up 0 min`).
  * Confirms header values, base addresses, page size, and kernel entry parameters are 100% accurate for the OnePlus 2 bootloader.

---

## Control Experiment: Raw Uncompressed ARM64 Image & Appended DTB Rule

Before writing any shim code, we tested the raw-Image path by decompressing the stock kernel gzip stream and appending the 7 DTBs directly:

### 1. Stock Kernel Extraction
* **Stock `kernel` binary**: 9,917,564 bytes (`Image.gz-dtb`)
* **Gzip Payload**: Offset `0x0` to `0x7be425` (8,119,333 bytes)
* **Appended DTBs**: Offset `0x7be425` to EOF (1,798,231 bytes, 7 DTBs)
* **Uncompressed `Image`**: 18,140,680 bytes (~17.30 MB)

### 2. First 64 Bytes of Uncompressed Stock Image
```text
0000:  10 00 00 14 00 00 00 00 00 00 08 00 00 00 00 00   ................
0010:  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0020:  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0030:  00 00 00 00 00 00 00 00 41 52 4d 64 00 00 00 00   ........ARMd....
```
* `code0`: `0x14000010` (`b 0x40` branch past 64-byte header)
* `code1`: `0x00000000`
* `text_offset`: `0x0000000000080000` (`0x80000`)
* `image_size`: `0x0000000000000000` (`0` in Linux 3.10 AArch64 v0 header)
* `flags`: `0x0000000000000000`
* `magic`: `0x644d5241` (`ARM\x64`)

### 3. Empirical Discovery: How Little Kernel Locates the DTB
1. **Test A (Raw Concatenation, `image_size = 0`)**:
   `fastboot boot out/test-uncompressed.img` $\rightarrow$ `FAILED (remote: 'dtb not found')`
2. **Test B (Populating `image_size` at offset 16 with file length)**:
   `fastboot boot out/test-uncompressed-with-size.img` $\rightarrow$ `FAILED (remote: 'dtb not found')`
3. **Test C (Writing DTB offset at offset `0x2C`)**:
   * Little Kernel (`platform/msm_shared/dev_tree.c`) function `dev_tree_appended()` reads `app_dtb_offset` from `kernel + DTB_OFFSET`, where `DTB_OFFSET = 0x2C` (legacy 32-bit zImage end offset).
   * In 64-bit arm64 Linux 3.10 headers, offset `0x2C` was zero (reserved), causing `dtb = kernel + 0` $\rightarrow$ failure!
   * We packed `uint32_t(len(Image)) = 0x0114ce08` at offset `0x2C` (byte 44) of `Image` and appended the DTBs at that offset.
   * `fastboot boot out/test-uncompressed-dtb-2c.img`:
     ```text
     Sending 'boot.img' (20260 KB)                      OKAY [  0.483s]
     Booting                                            OKAY [  0.079s]
     Finished. Total time: 0.565s
     ```
   * **Result: Android 10 booted up completely to userspace!**
   * **Rule Confirmed**: For any uncompressed ARM64 image on OnePlus 2, the exact byte offset where the appended DTBs start **MUST be written at offset `0x2C`**!

---

## Ramoops Log Persistence Verification
Checked on live device:
* `[ 0.000000] OEM: Found ramoops_mem, memory base 0x00000000ac000000, size 1 MiB`
* `[ 0.235146] ramoops: attached 0x100000@0xac000000, ecc: 0/0`
* Sysfs `/sys/fs/pstore`: `console-ramoops-0` (262 KB) and `pmsg-ramoops-0` (4 KB) exist and survive warm reboots!

---

## M1 Bare-Metal Boot Shim Verification (Day 2)

### 1. Implementation
* **Entry & Vectors**: `shim/start.S` implements ARM64 Image header with Little Kernel DTB offset dynamically injected into byte `0x2C`. Complete 16-slot exception vector table aligned to `0x800` catching synchronous aborts, IRQ, FIQ, and SError.
* **Stack**: Dedicated safe stack pointer initialized to `sp = 0x00400000` (growing downward towards `0x00300000`), completely isolated from kernel, DTB, ramdisk, and splash memory.
* **Persistent Ramoops Console**: Buffer initialized at physical address `0xac0be000` with signature `0x43474244` (`DBGC`).
* **Display Test**: Dual-format color stripe pattern painted directly to continuous splash memory at `0x03401000`:
  * Top half (lines 0..959): 32 bpp, stride 4352 bytes (Red, Green, Blue bands).
  * Bottom half (lines 960..1919): 24 bpp, stride 3240 bytes (Yellow, Cyan, Magenta bands).
* **Reset**: Deasserts PS_HOLD by writing `0` to `0xfc4ab000` after a 5-second `cntvct_el0` delay loop.
* **Toolchain & Build**: Automated build script [`shim/build.py`](shim/build.py) / [`shim/build.sh`](shim/build.sh) using `clang -target aarch64-none-elf` and `ld.lld`.

### 2. Live Boot Test & Execution Evidence
Executed `fastboot boot out/shim-boot.img`:
```text
Sending 'boot.img' (2560 KB)                       OKAY [  0.077s]
Booting                                            OKAY [  0.178s]
Finished. Total time: 0.255s
```

After device warm-reset and boot into Android, retrieved `/sys/fs/pstore/console-ramoops-0`:
```text
=======================================================
  [ONEPLUS2-SHIM] M1 Bare-Metal Shim Executed Successfully!
=======================================================
  Execution Level : EL01
  FDT Pointer (x0): 0x0000000001E00000
  FDT Magic       : 0xD00DFEED

  Painting Framebuffer stripes (32bpp top, 24bpp bottom)... DONE
  Holding display for 5 seconds...
  Triggering PMIC reset via PS_HOLD (0xfc4ab000)...
=======================================================
```

### 3. Key Facts Confirmed
1. **Bare-metal execution verified**: Little Kernel transfers control cleanly to our uncompressed shim.
2. **Execution Level**: The CPU executes at **EL1** (`CurrentEL = 0x4` $\rightarrow$ EL1).
3. **Little Kernel DTB passing location**: LK places the patched FDT at **`0x01E00000`** (30 MiB mark in RAM).
4. **FDT validity**: Magic bytes at `0x01E00000` verified as `0xd00dfeed`.
5. **Ramoops channel operational**: The 256 KB console buffer at `0xac0be000` survives warm PMIC resets and dumps cleanly into Linux userspace.
6. **PS_HOLD software reset**: Writing 0 to `0xfc4ab000` triggers immediate SoC reset.

---

## M1c: Extended C Runtime, FDT Parser & Display Pipeline Analysis

### 1. Extended C Runtime Execution
Integrated freestanding C runtime: [`shim/main.c`](shim/main.c), [`shim/console.c`](shim/console.c), [`shim/fdt.c`](shim/fdt.c), [`shim/font.h`](shim/font.h).
Executed `fastboot boot out/shim-boot.img` with live 20-second countdown loop and full FDT `/chosen` inspection.
Retrieved `/sys/fs/pstore/console-ramoops-0`:
```text
=======================================================
  [ONEPLUS2-SHIM] M1 On-Screen Text Console Active!
=======================================================
  CurrentEL: EL1, FDT: 0x0000000001E00000
  Initrd start: 0x0000000002000000
  Initrd end:   0x00000000020C3E6B
  Bootargs: sched_enable_hmp=1 sched_enable_power_aware=1 androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x37 ehci-hcd.park=3 lpm_levels.sleep_disabled=1 boot_cpus=0-3 loop.max_part=7 androidboot.selinux=permissive buildvariant=userdebug
  Countdown: Rebooting into Android in 20s... 
  Countdown: Rebooting into Android in 19s... 
  ...
  Countdown: Rebooting into Android in 01s... 
  Countdown: Rebooting into Android in 00s... 

[SHIM] Resetting SoC via PS_HOLD (0xfc4ab000)...
```

### 2. Ramdisk & Memory Findings
* **Ramdisk Relocation**: Little Kernel relocates the ramdisk to **`0x02000000`** (`32 MiB` physical offset), spanning to `0x020C3E6B` (802,411 bytes).
* **Headroom**: The space between `0x02000000` and the display reservation at `0x03401000` provides **20 MiB** of contiguous memory for XNU boot payloads without clobbering memory.
* **Bootargs**: LK dynamically injected `sched_enable_hmp=1 sched_enable_power_aware=1` into `/chosen/bootargs`.

### 3. Display Subsystem Reverse-Engineering & Explanation
During the 20-second countdown, the physical display remained on the static OnePlus boot logo.
Disassembly and reverse-engineering of the device's native `backup/aboot.img` revealed the exact hardware reason:
1. **Command Mode Panel**: The OnePlus 2 panel is `14049_JD35695_1080P_CMD_PANEL` (`jd35695 1080p cmd mode dsi panel`), which contains internal Graphic RAM (GRAM) with autonomous self-refresh.
2. **`target_display_shutdown()`**: In Qualcomm LK `boot_linux()`, right before calling TrustZone (`0x02001001`) to jump to the 64-bit kernel, LK invokes `target_display_shutdown()`, which powers off the MIPI-DSI host controller and display clocks.
3. **Continuous Splash Not Passed**: In Linux dmesg, `mdss_mdp_splash_parse_dt: splash mem child node is not present` confirms that continuous splash is intentionally not passed from LK on this target.
4. **Conclusion**: Writing to DRAM at `0x03401000` without reinitializing the DSI host controller and sending a DSI command-mode kickoff (`MDSS_MDP_REG_CTL_START` / `0xfd90201c`) does not update the panel's internal GRAM. The panel simply continues self-refreshing the 1+ logo from its own internal GRAM.
5. **Debug Channel**: **Ramoops (`0xac0be000`) is confirmed as the primary, 100% reliable bare-metal debug console** for all subsequent milestones (M2 through M5).


