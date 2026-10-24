# iOS on OnePlus 2 (Snapdragon 810 / MSM8994)

An experimental project to port Apple's open-source XNU kernel and iOS components to the **OnePlus 2** (`oneplus2` / `ONE A2003`), based on the research and methodology of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet).

## Architecture Differences from `ios-sweet`

Because the OnePlus 2 uses the **Snapdragon 810 (MSM8994)** rather than the Snapdragon 732G:
1. **MMU Page Size**: Cortex-A57/A53 hardware only supports **4 KB & 64 KB** pages (no 16 KB support). This port targets the **iOS 12.5.7 (iPhone 6 / iPhone 5s)** generation, which natively uses 4 KB-aligned Mach-O binaries and 4 KB page tables.
2. **Interrupt Controller**: Uses **ARM GICv2** memory-mapped registers rather than GICv3 system registers.
3. **Storage**: Uses **eMMC 5.0** (Qualcomm SDCC/SDHCI controller) or a RAM disk (`mockfs`) rather than UFS.
4. **Bootloader**: Uses Qualcomm's **Little Kernel (LK)** with Android `boot.img v0`.

## Repository Structure

- `tools/`: Utility scripts for packaging, unpacking, and building payloads.
- `shim/`: Bare-metal ARM64 boot shim.
- `xnu/`: Kernel patches and build configurations.
- `NOTES.md`: Detailed engineering log and hardware facts.
- `backup/`: (Git-ignored) Calibration and partition backups.
- `dtb/`: (Git-ignored) Extracted device tree blobs.
- `recovery/`: (Git-ignored) Temporary recovery images.

Refer to [NOTES.md](file:///Users/veerhooda/Downloads/OnePlus%202/NOTES.md) for the active engineering log and hardware specifications.
