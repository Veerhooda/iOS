# Credits & Attributions

This project builds upon open-source research and engineering across the Darwin, Linux, and mobile reverse engineering communities.

### 1. `ios-sweet` Project (dipa-studio)
- **Init Process & Mach-O Packaging:**  
  The position-independent static ARM64 init template (`tools/op2-init.S`) and static Mach-O code-signing wrapper (`tools/mkinit.py`) are derived from `sweet-init.S` and `mkinit.py` developed by the [ios-sweet](https://github.com/dipa-studio/ios-sweet) project (porting Darwin/XNU to the Xiaomi Redmi Note 10 Pro / SM7150).
- **HFS Integration Reference:**  
  The architectural blueprint for embedding Apple's open-source HFS into a monolithic XNU kernel without loadable kexts (`sweet_hfs_iokit.c` and compatibility shims) references the `ios-sweet` patch series.

### 2. Apple Inc. (Apple Open Source)
- **Darwin / XNU Kernel:**  
  Based on `xnu-4903.270.47` (macOS 10.14.6 / iOS 12.5.7).
- **libpthread:**  
  In-kernel pthread registration adapted from `libpthread-330.250.2` and `libpthread-514.100.2`.
- **HFS Filesystem:**  
  Apple Open Source `hfs-407.200.4`.

### 3. Qualcomm & OnePlus Open Source
- **Linux Kernel Source:**  
  OnePlus 2 Android 6.0/7.1.1 kernel source tree (`kernel-oneplus-msm8994`) for hardware register definitions, clock trees, pin control, and device tree bindings.
