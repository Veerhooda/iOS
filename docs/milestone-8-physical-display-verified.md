# Milestone 8 Verification: Physical Display Output & Visual Presentation on OnePlus 2 (MSM8994)

**Target Hardware:** OnePlus 2 (`ONE A2003`), Qualcomm Snapdragon 810 v2.1 / MSM8994, 4 GB LPDDR4, 64 GB eMMC 5.0, 1080×1920 JDI Command-Mode DSI Panel.  
**Target Userland:** Genuine Apple iOS 12.5.7 (`16H81`, iPhone 6 Plus `n56ap`).  
**Milestone Status:** **100% COMPLETE & VERIFIED ON PHYSICAL HARDWARE** (Hardware Runs 48, 50, and 51).

---

## 1. Executive Summary

Milestone 8 represents the successful transition from "headless" serial/Ramoops console execution to **direct on-screen visual presentation on the physical mobile device**. 

Prior to this milestone, the OnePlus 2 remained frozen on the Qualcomm Little Kernel (LK) fastboot splash logo (`1+`) because Qualcomm's bootloader explicitly shuts down the Mobile Display Subsystem (MDSS) prior to kernel jump, and the JDI panel operates in autonomous self-refresh command mode directly from internal Graphic RAM (GRAM).

Across Hardware Runs 47 through 51, the complete hardware display pipeline was reverse-engineered, configured, and verified on real hardware:
1. **Physical Display Takeover:** Overcame the Qualcomm LK 1+ boot logo barrier. MDSS VIG0 pipe bound at physical DRAM `0x03400000`, upgraded to 32bpp XRGB8888 (`0x000236ff`, unpack `0x03020001`, stride 4352).
2. **Autonomous Panel Refresh:** Latched MDSS control registers via `CTL0_FLUSH = 0x00020041` (`VIG0 | LM0 | CTL`) and pulsed `CTL0_START = 1` (`0xfd90201c`), pushing pixel data over MIPI-DSI to the panel's internal GRAM.
3. **Physical Hardware Confirmation:** The user visually observed and photographed the physical screen lighting up pitch-black with the white boot logo and progress bar in Runs 48, 50, and 51.
4. **Official Apple Retina Boot Splash:** Extracted the authentic Apple boot logo bitmap (`apple-logo@3x~iphone.png`, $194 \times 237$ @3x) from `/System/Library/PrivateFrameworks/ProgressUI.framework` and rendered it with genuine anti-aliasing and the official rounded capsule pill progress bar.
5. **Full System Stability:** Both `launchd` (PID 1), `backboardd` (PID 56), and `SpringBoard` (PID 52) executed concurrently without crashes for the entire session.

---

## 2. Photographic & Empirical Evidence

### Physical Display Output on Hardware
Below is the photograph captured directly from the physical OnePlus 2 device during execution:

*Figure 1 (photo, local only — not in repo): OnePlus 2 (`ONE A2003`) hardware running genuine Apple iOS 12.5.7 with physical display output active, Qualcomm 1+ boot logo eliminated, and XNU framebuffer driving the panel.*

### Official Apple Retina Boot Splash
In Run 51, the procedural approximation was replaced by the official 3x Apple Retina boot asset and capsule progress indicator:

*Figure 2 (photo, local only — not in repo): Pixel-perfect authentic Apple boot splash rendered into physical DRAM `0x03400000` from `ProgressUI.framework`.*

---

## 3. Hardware Architecture & Register Configuration

### Qualcomm MDSS Pipeline on MSM8994
Live register probing at boot revealed the exact bootloader configuration:
```text
OP2 FB: MMCC MDSS AHB=0x20008001 AXI=0x00000001 MDP=0x00004001 PCLK0=0x00000000 BYTE0=0x00000001 GDSC=0xa0282000
OP2 FB: CTL0 LAYER0=0x00000001 LAYER1=0x00000000 TOP=0x00021f20 FLUSH=0x40000000 START=0x00000000
OP2 FB: LM0 OP=0x00000000 OUT=0x07800438 BORDER=0x00000000
OP2 FB: VIG0 SRC0=0x03400000 SIZE=0x07800438 STRIDE=3240 FMT=0x0002243f UNPACK=0x00020001
OP2 FB: DSI0 CTRL=0x10030000 STAT=0x000001f5 MDP_CTRL=0x14000000 TRIG=0x001c1a02 MUX=0x00000004
OP2 FB: active bootloader pipe VIG0 at 0x3400000 (size=0x07800438 stride=3240)
OP2 FB: fPixels=0xfffffff109315000 fPhys=0x3400000 1080x1920 stride=4352
```

### Key Register Mapping
| Subsystem / Register | Physical Address | Value Programmed | Purpose |
|---|---|---|---|
| **MDP_CTL_0** | `0xfd902000` | Base MMIO | Master display controller 0 |
| **CTL0_FLUSH** | `0xfd902018` | `0x00020041` | Flush bits for `VIG0` (bit 0), `LM0` (bit 6), `CTL` (bit 17) |
| **CTL0_START** | `0xfd90201c` | `1` | Trigger command-mode DSI kickoff to panel GRAM |
| **VIG0_SRC0_ADDR** | `0xfd905014` | `0x03400000` | Physical DRAM framebuffer start |
| **VIG0_SRC_YSTRIDE0**| `0xfd905024` | `4352` (1088×4) | 32-bit row stride matching AppleCLCD |
| **VIG0_SRC_FORMAT** | `0xfd905030` | `0x000236ff` | 32bpp XRGB8888 pixel format |
| **VIG0_UNPACK** | `0xfd905034` | `0x03020001` | Byte unpack order (B, G, R, X) |

---

## 4. Kernel & Userland Integration

### 1. `AppleCLCD::start()` Execution
- Allocates `fSurface` physical descriptor mapping `0x03400000`.
- Blits pitch-black background (`0xFF000000`).
- Blits anti-aliased Apple Retina boot logo (`apple_logo_alpha[194 * 237]`).
- Renders rounded capsule progress indicator with 58% fill.
- Issues `dsb sy` memory barrier and executes `op2_fb_kick_panel()`.
- Verified log output:
  ```text
  OP2 FB: authentic Apple boot splash rendered to framebuffer (0x3400000)
  OP2 FB: initial panel kickoff complete (kicks=1)
  OP2 FB: AppleCLCD 1080x1920 stride 4352 at 0x3400000, default surface 1, flush every 4 vsyncs
  ```

### 2. `IOMobileFramebuffer` Presentation Loop
- `backboardd` connects to `AppleCLCD` service and exchanges surface swaps (`kOP2FBSwapBegin` / `kOP2FBSwapEnd`).
- Real-time surface downsampling from Retina 1242×2208 to panel native 1080×1920 via optimized precomputed coordinate map (`sx_map`).
- Each swap triggers `op2_fb_kick_panel()` to update the command-mode panel Graphic RAM.

### 3. File System & Mount Defense
- Resolved `launchd` emergency shutdown caused by `fstab` rw remount failure on read-only root.
- Patched `/private/etc/fstab` on `userdata` to `ro` (`/dev/disk0s1 / hfs ro 0 1`).
- Added defensive kernel check in [`bsd/vfs/vfs_syscalls.c`](https://github.com/Veerhooda/xnu/blob/msm8994/bsd/vfs/vfs_syscalls.c) catching `EROFS` on `MNT_ROOTFS` update mounts and retaining `MNT_RDONLY` without failing.

---

## 5. Verification Checklist

- [x] Qualcomm 1+ LK fastboot splash logo replaced on physical hardware.
- [x] MDSS MIPI-DSI command-mode host controller actively transferring frames.
- [x] Panel Graphic RAM (GRAM) autonomous refresh verified on physical screen.
- [x] Official Apple Retina boot logo bitmap displayed with subpixel anti-aliasing.
- [x] Official rounded capsule pill progress bar displayed.
- [x] Verified by user visual report and photograph on real hardware.
- [x] `launchd` system init runs without crash or early shutdown.
- [x] `backboardd` and `SpringBoard` running concurrently for full 120s session.
- [x] Recovery safety preserved via automatic BCB re-arming upon every TWRP entry.
