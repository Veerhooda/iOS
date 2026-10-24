# iOS on OnePlus 2 (Snapdragon 810 / MSM8994) - Engineering Notes

## Project Overview
Porting Apple's XNU kernel and iOS userland components to the OnePlus 2 (`oneplus2` / `ONE A2003`, Qualcomm Snapdragon 810 / MSM8994), inspired by [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note 10 Pro / SM7150).

---

## Hardware & Architecture Profile (OnePlus 2 vs. sweet)

| Component | OnePlus 2 (`oneplus2`) | sweet (`SM7150`) | Architecture & Porting Strategy |
| :--- | :--- | :--- | :--- |
| **SoC** | Qualcomm Snapdragon 810 (MSM8994) | Snapdragon 732G (SM7150) | Older big.LITTLE architecture |
| **CPU Cores** | 4x Cortex-A57 + 4x Cortex-A53 | 2x Cortex-A76 + 6x Cortex-A55 | Heterogeneous ARMv8.0-A |
| **ISA** | ARMv8.0-A | ARMv8.2-A | No ARMv8.1 LSE atomics (`CAS`, `LDADD`) in hardware |
| **MMU Granule** | **4 KB & 64 KB only** (`TGran16 == 0x0`) | 16 KB & 4 KB (`TGran16 == 0x1`) | **Targeting iOS 12.5.7 (iPhone 6 / 5s)** which uses native 4 KB pages (`ARM_PGSHIFT = 12`) |
| **Interrupts** | **ARM GICv2** (`qcom,msm-qgic2`) | ARM GICv3 (System Regs `ICC_*`) | Memory-mapped CPU interface (`0xf9002000`) & distributor (`0xf9000000`) |
| **Storage** | **eMMC 5.0** (Qualcomm SDCC/SDHCI) | UFS 2.2 (UFSHCI) | Need SDHCI driver in XNU, or boot early userland via RAM disk (`mockfs`) |
| **Display** | 1080×1920 (16:9), MDSS DSI | 1080×2400 (20:9), DSI | Matches iPhone 6 Plus 1080p native target; continuous splash in RAM |
| **Bootloader** | Qualcomm Little Kernel (LK) | Qualcomm ABL (UEFI) | Android `boot.img` v0 format; base `0x00000000`, kernel `0x00080000`, tags `0x00000100` |
| **RAM** | 3 GB or 4 GB LPDDR4 | 6 GB / 8 GB LPDDR4X | Full address space `< 4 GB`; carve-out mapping discipline required |

---

## Day 1: Setup & Environment Verification Log

### 1. Host Machine & Connection
* **Host OS**: macOS Darwin (arm64, Apple Silicon). Direct USB connection (native USB stack, avoiding WSL/VM pass-through flakiness).
* **Connection Status**: USB connection stable, device recognized in fastboot and adb.
* **Cable & Port**: Direct USB connection verified.

### 2. Connected Device Verification
* **Command**: `adb devices`
  * Result: `ebd7ee57    device`
* **Device Model**:
  * `ro.product.model`: `ONE A2003` (OnePlus 2)
  * `ro.product.device`: `OnePlus2`
  * `ro.build.version.release`: `10`
* **Battery Level Check**:
  * `adb shell cat /sys/class/power_supply/battery/capacity`
  * Result: **61%** (verified > 50% safety requirement)

### 3. Toolchain & Tool Status
* `adb`: `/opt/homebrew/bin/adb` (Android Debug Bridge 35.0.2)
* `fastboot`: `/opt/homebrew/bin/fastboot`
* `dtc`: `/opt/homebrew/bin/dtc` (Device Tree Compiler 1.7.2)
* `qemu-system-aarch64`: `/opt/homebrew/bin/qemu-system-aarch64` (QEMU 10.2.0)
* `lldb`: `/usr/bin/lldb` (Apple LLVM debugger with aarch64 & `gdb-remote` target support)
* `python3`: `/usr/bin/python3` (Python 3.9.6) and `/opt/homebrew/bin/python3`
* `clang`: `/usr/bin/clang` (tested: `clang -target aarch64-none-elf` compiles bare-metal aarch64 ELF objects cleanly)
* `mkbootimg.py`: Staged in `tools/mkbootimg.py` (AOSP / LineageOS implementation)
* `unpack_bootimg.py`: Staged in `tools/unpack_bootimg.py`
* `unpack_bootv2.py`: Staged in `tools/unpack_bootv2.py`
* `adt.py`: Staged in `tools/adt.py`

### 4. Repository Configuration
* Initialized git repository.
* Configured `.gitignore` to prevent committing sensitive device calibration data, partition dumps, and serial numbers:
  * Excluded: `backup/`, `dtb/`, `recovery/`, `reference/`, `research/`, `logs/`, `out/`, `*.img`, `*.simg`, `*.bin`, `*.elf`, `*.o`, `*.dtb`.

---

## Command History & Verification Record

| Timestamp | Command | Exit Code | Observation / Output |
| :--- | :--- | :--- | :--- |
| 2026-10-24 19:09 | `which adb fastboot dtc qemu-system-aarch64 ...` | 1 | adb, fastboot, dtc, qemu, python3, lldb found. |
| 2026-10-24 19:10 | `adb devices; fastboot devices` | 0 | Found device `ebd7ee57`. |
| 2026-10-24 19:12 | `adb shell getprop ro.product.model ...` | 0 | Confirmed `ONE A2003` (`OnePlus2`), Android 10, Battery 61%. |
| 2026-10-24 19:13 | `echo "void _start() { while(1); }" \| clang -target aarch64-none-elf ...` | 0 | Verified clang can generate aarch64 ELF binaries. |
| 2026-10-24 19:18 | `curl ... LineageOS/.../mkbootimg.py` | 0 | Downloaded standalone `mkbootimg.py`. |
| 2026-10-24 19:21 | `curl ... LineageOS/.../unpack_bootimg.py` | 0 | Downloaded standalone `unpack_bootimg.py`. |
| 2026-10-24 19:22 | `git init` | 0 | Git repository initialized with clean `.gitignore`. |

---

## Next Steps (Day 2)

1. **Partition Backups**:
   * Boot into recovery (e.g. TWRP for `oneplus2`) temporarily via `fastboot boot recovery.img`.
   * Pull critical partitions to `backup/`:
     * `persist`
     * `modemst1`, `modemst2`, `fsg`, `fsc` (contains IMEI, baseband NVRAM and radio calibration)
     * `boot`
     * `recovery`
2. **Extract Live Kernel & Device Tree (DTB)**:
   * Unpack the stock/working `boot.img` using `tools/unpack_bootimg.py` into `dtb/boot_unpacked`.
   * Extract the Qualcomm QCDT/DTB containing MSM8994 memory map, continuous splash address, interrupt numbers, and peripheral base addresses.
3. **Begin Bare-Metal Boot Shim**:
   * Port `shim/` to use a 4 KB MMU translation table.
   * Target Little Kernel `boot.img` v0 header.
   * Validate early framebuffer console on the OnePlus 2 screen.
