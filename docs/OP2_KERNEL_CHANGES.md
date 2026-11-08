# OP2 Kernel Changes (`xnu/xnu-4903.270.47`, branch `msm8994`)

The XNU tree is a **nested repository** (`xnu/xnu-4903.270.47`) tracking Apple's open-source Darwin kernel (`xnu-4903.270.47`, macOS 10.14 / iOS 12 era). The remote is `apple-oss-distributions/xnu`, and all OnePlus 2 port modifications are committed locally on branch `msm8994`.

This document indexes all commits and technical modifications made to the kernel source tree, serving as a comprehensive architectural and audit reference.

---

## Committed OP2 Git History (`msm8994` Branch)

| Commit Hash | Milestone / Feature | Core Summary |
| :--- | :--- | :--- |
| `12623e4` | Config Base | Initial custom MSM8994 (OnePlus 2) board config (`ARM64_BOARD_CONFIG_MSM8994`). |
| `864a47b`…`2304585` | Toolchain & Bringup | Xcode 10.3 & Apple Silicon clang fixes, PL011 UART + Ramoops serial logger, PS_HOLD reset on panic, IMEM recovery restart cookie (`0x77665502`), 4 KB page enforcement (`ARM_PGSHIFT=12`), and early memory diagnostics. |
| `615ac4a` | Milestone 3 | Built-in `libpthread-330.250.2` backported with ARM64 thread state, corecrypto null-guards, `OP2PlatformExpert` device tree matching (`N56AP`), and early root discovery. |
| `94dfb07` | Milestone 4 | Staged Apple open-source `hfs-627.100.6` filesystem driver into kernel with macOS 10.14 compatibility shims; static RAM-disk root boot. |
| `9c526b9` | Milestone 4b | ARM GICv2 interrupt controller driver (`OP2InterruptController`) and ARM Generic Virtual Timer (`CNTV_CTL_EL0`, `CNTV_TVAL_EL0`) 10 ms periodic tick dispatch. |
| `dab30c6` | Milestone 5 | `op2_emmc` block device driver (SDHCI PIO) and persistent HFS+ root mount from eMMC 5.0 flash storage (`userdata` partition). |
| `d59391e` | Milestone 6 | `AppleKeyStore` stubs, `IOWatchdog`, redundant VFS mount bypass, and launchd process execution bootstrap. |
| `921f634` | Logging Optimization | Concise syscall tracing to conserve the 256 KiB Ramoops circular ring buffer. |
| `4444662` | Milestone 9 | Dual-core SMP bring-up (`cpus=2`), dedicated `OP2Buttons` Consumer HID service, zero-fill display frame preservation, RGB alpha masking, and 256MB jetsam floor. |
| `195e98c` | **Milestone 10 (HEAD)** | **Phase 1 Metal Bring-Up**: Published `IOAcceleratorES` and `AGXAccelerator` services attached to AppleCLCD for Metal userland plugin discovery. |

---

## Subsystem Architecture & Implementation Details

### 1. Dual-Core SMP Architecture (`osfmk/arm64/start.s`, `osfmk/arm/arm_init.c`, `iokit/Kernel/arm/OP2CPU.cpp`)
- **Secondary CPU Release Mechanism:**
  - Little Kernel leaves secondary cores parked in a low-power WFI spin-table loop in secure memory.
  - Secondary core bring-up is triggered via ARM SMC PSCI call `CPU_ON` (`0xC4000003`), supplying target CPU MPIDR (`0x0001` for CPU 1) and physical entry point address (`arm_init_secondary`).
  - Secondary CPU mailbox status is signaled via uncached physical memory synchronization (`0x00080000` / `0x00080008`).
- **Secondary Kernel Entry (`start.s`):**
  - CPU 1 vectors to `arm_init_secondary` in EL1.
  - Configures CPU 1 architectural system registers (`CPACR_EL1` enabling FP/SIMD, `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, `MAIR_EL1`).
  - Sets up dedicated secondary CPU interrupt and kernel exception stacks allocated by `OP2CPU`.
  - Enables MMU via `msr sctlr_el1, x0` followed by `isb sy`.
- **Interrupts & Cross-Core Communication:**
  - Configures GICv2 CPU interface registers (`GICC_CTLR`, `GICC_PMR`, `GICC_BPR`) for CPU 1.
  - Re-arms per-core ARM Generic Virtual Timer (`CNTV_CTL_EL0`).
  - Software Generated Interrupt (SGI 0) dispatched via `GICD_SGIR` serves as cross-core Inter-Processor Interrupt (IPI) for AST rescheduling, debugger entry, and TLB invalidation shootdowns.

### 2. Display Engine & Frame Preservation (`iokit/Kernel/arm/OP2Framebuffer.cpp`, `OP2Surface.cpp`)
- **Physical Panel Topology:**
  - MDSS MDP5 command-mode DSI panel at 1080×1920 resolution, 32-bit ARGB/BGRA, 4352-byte padded row stride (1088 words).
  - Mapped at continuous splash physical base `0x03401000`.
- **Dynamic Surface Scanning:**
  - iOS 12 `backboardd` allocates multiple off-screen compositing surfaces for lockscreen, wallpapers, and SpringBoard icons.
  - Driver dynamically scans userland surfaces up to ID 63, detecting active surface transitions (Surfaces 2, 9, 12, 15, 18, 21).
  - Retina frames rendered at 1242×2208 are bilinearly downsampled to native 1080×1920 in real time.
- **RGB Alpha Masking Fix:**
  - In BGRA 32bpp, opaque black background is represented as `0xFF000000` (alpha=255, red=0, green=0, blue=0).
  - Previous sampling evaluated `row[x] != 0`, falsely treating black blanking frames as non-zero graphics.
  - Updated to `(row[x] & 0x00FFFFFF) != 0` to accurately evaluate true RGB content.
- **Zero-Fill Frame Preservation:**
  - When `backboardd` issues composite swap commands targeting an uninitialized or empty surface (e.g. Surface 5 during mode switches), `AppleCLCD::present` freezes the existing non-zero frame buffer in physical panel memory (`fPixels`).
  - Permanently cures display blanking regressions, with 100% of sampled panel pixels remaining non-zero (`panel_nz = 2000/2000`) over 30-minute soaks.
- **Power Veto:**
  - Intercepts display power state transitions in `IOPMrootDomain.cpp` and `OP2Framebuffer.cpp`, vetoing display sleep commands (`op2fb_keepon=1`).

### 3. Dedicated `OP2Buttons` Consumer HID Service (`iokit/Kernel/arm/OP2Digitizer.cpp`)
- **The Challenge:**
  - In iOS 12, `backboardd` ignores Home button key events if published through a digitizer/touchscreen HID device interface.
- **The Solution:**
  - Declares an independent IOHIDEventService registering as Consumer/ConsumerControl (Usage Page `0x0C`, Usage `0x40`).
  - Binds directly to `SpringBoard`'s Mach runloop.
  - Dispatches simulated AUTOHOME events (`op2_tapms`) and physical capacitive sensor presses to reliably wake the display, dismiss lockscreen UI, and navigate to the application icon grid.

### 4. 256MB Jetsam Footprint Floor (`osfmk/kern/task.c`)
- **The Problem:**
  - Legacy `n56ap` (iPhone 6 Plus) device profiles configure system memory limits tailored for 1 GB total DRAM. Critical daemons (`assistivetouchd`, `quicklookd`) were assigned 36 MB footprint ceilings.
  - On the OnePlus 2 (4 GB physical DRAM), minor memory allocations during SpringBoard startup triggered fatal out-of-memory jetsam kills.
- **The Solution:**
  - In `task_set_phys_footprint_limit_internal()`, enforced a mandatory clamp:
    ```c
    #if defined(ARM64_BOARD_CONFIG_MSM8994)
    if (new_limit > 0 && new_limit < (256 * 1024 * 1024)) {
        new_limit = 256 * 1024 * 1024; // 256MB floor
    }
    #endif
    ```
  - Prevents premature daemon deaths while preserving over 1.8 GB of completely free DRAM.

### 5. SDHCI Storage Subsystem Hardening (`bsd/dev/op2_emmc.c`)
- **Hardware Architecture:**
  - Qualcomm SDCC 5.0 controller mapped at `0xf9824900` (HC) and `0xf9824000` (Core Vendor).
- **Resilience Features:**
  - Automated `CMD12` (STOP_TRANSMISSION) abort handling upon block read/write timeouts.
  - Explicit DAT0 busy-line polling before and after block transfers.
  - 3x automatic retry loop on CRC and timeout error conditions (`retry_ok=1`).
  - Zero read or write errors across 1,360,000+ operations in prolonged stability tests.

### 7. Qualcomm RPM SMD Client Protocol & Adreno 430 Silicon Bring-Up (`bsd/dev/op2_rpm.c`, `iokit/Kernel/arm/OP2GPU.cpp`)
- **RPM SMD Co-Processor Client:**
  - Implements Qualcomm Shared Memory (SMEM) channel allocation (`cid=4`, type `0x0000020f`, `rpm_requests`).
  - Two-phase SMD open handshake with stale bootloader packet synchronization.
  - Votes on PMI8994 S2 (`VDD_GFX`) PMIC buck regulator in both Active Set (`set=0`) and Sleep Set (`set=1`) with verified RPM ACKs (`id_ack=2`, `id_ack=3`).
- **MMSSCC Power & Clock Control:**
  - Uncollapses Oxili GDSC power domains (`oxili_cx` and `oxili_gx`), confirming `PWR_ON` assertion within 10 µs.
  - Enables MMSS branch clocks (`oxilicx_ahb`, `oxili_gfx3d`, `oxili_rbbmtimer`).
- **Adreno 430 Silicon Initialization:**
  - Core soft reset via `A4XX_RBBM_SW_RESET_CMD` (`status=0xc0004081`).
  - Microcode load of 3,460 words (PM4 + PFP) directly to CP instruction memory via AHB MMIO.
  - 32 KB contiguous DDR ringbuffer allocation with ARM64 `dc civac` PoC cache flushing.
  - Verified DMA execution of `CP_ME_INIT` and `CP_NOP` canary packets.

### 8. `OP2GPUUserClient` & Userspace Hardware Command Dispatch (`iokit/Kernel/arm/OP2GPU.cpp`, `OP2GPU.h`)
- **Linux `msm_drm` ABI Compatibility:**
  - Subclasses `IOUserClient` to provide ioctl-compatible external method dispatch for userland tasks:
    - `kMsmGetParam`: Queries `MSM_PARAM_CHIP_ID` (`0x04030002` Adreno 430 v2.1), `MSM_PARAM_GMEM_SIZE` (`0x180000` / 1.5 MB), `MSM_PARAM_MAX_FREQ` (`600 MHz`), and timestamps.
    - `kMsmGemNew`: Allocates contiguous physical DDR memory via `IOBufferMemoryDescriptor`.
    - `kMsmGemInfo`: Returns physical IOVA address.
    - `kMsmGemCpuPrep` / `kMsmGemCpuFini`: Point-of-Coherency cache flush with ARM64 `dc civac`.
    - `kMsmGemSubmit`: Dispatches PM4 Type 3 indirect buffer packets (`CP_INDIRECT_BUFFER_PFE`) over DDR DMA.
    - `kMsmWaitFence`: Synchronous fence completion tracking with microsecond polling.
    - `kDrmGemClose`: Clean teardown of memory descriptors and fence synchronization.
- **Mach VM Contiguous Memory Mapping:**
  - Implements `clientMemoryForType` enabling userspace tasks to map physical GEM descriptors into task VM space via `IOConnectMapMemory64`.
- **Unified `AGXAccelerator` Routing:**
  - Overrides `AGXAccelerator::newUserClient` in `OP2Framebuffer.cpp` to forward user client connections to `OP2GPU`, linking Apple `Metal.framework` directly to physical Adreno silicon.

---


## Build Instructions

```bash
# Compile the custom kernel (requires Xcode 10.3 clang toolchain)
./tools/build-xnu.sh

# Copy kernel image to out directory
cp xnu/xnu-4903.270.47/BUILD/obj/DEVELOPMENT_ARM64_MSM8994/kernel.development.msm8994 \
   out/kernel.development.msm8994
```

> [!IMPORTANT]
> The nested repository `xnu/xnu-4903.270.47` must never be pushed to `apple-oss-distributions/xnu`. All work must be committed exclusively to the local `msm8994` branch or a dedicated personal fork.
