# Master Knowledge Base & Engineering Handover: iOS 12.5.7 on OnePlus 2 (MSM8994)

**Target Hardware:** OnePlus 2 (`oneplus2` / `ONE A2003`), Qualcomm Snapdragon 810 v2.1 (`MSM8994`), Adreno 430, 4 GB LPDDR4, 64 GB eMMC 5.0  
**Host Environment:** macOS on Apple Silicon arm64, Xcode 10.3 toolchain at `$HOME/xcode-10.3`
(override: `XCODE_ROOT`; see `tools/env.sh` and `docs/SETUP.md`), Homebrew LLVM (`clang`, `ld.lld`,
`llvm-objcopy`), Android Platform Tools (`adb`, `fastboot`)  
**Operating System:** Apple iOS 12.5.7 (Build `16H81`), genuine `n56ap` (iPhone 6 Plus) dyld shared cache  
**Kernel:** Darwin/XNU `xnu-4903.270.47` (`DEVELOPMENT_ARM64_MSM8994`), native 4 KB page granule  
**Current Milestone:** **Milestone 26 (Verified on Physical Silicon, 50-Minute Soak Pass)**  
**Last Updated:** November 11, 2026  

---

## Table of Contents
1. [Core Safety Rules & Invariant Guardrails](#1-core-safety-rules--invariant-guardrails)
2. [Hardware Specifications & Physical Memory Map](#2-hardware-specifications--physical-memory-map)
3. [Boot Chain & Bare-Metal Shim Architecture](#3-boot-chain--bare-metal-shim-architecture)
4. [SMP & Multicore CPU Management](#4-smp--multicore-cpu-management)
5. [Storage Architecture & eMMC Driver](#5-storage-architecture--emmc-driver)
6. [Display Pipeline & High-Speed Frame Downsampler](#6-display-pipeline--high-speed-frame-downsampler)
7. [Graphics & Metal Acceleration Architecture](#7-graphics--metal-acceleration-architecture)
8. [Input, Touch & Button Subsystem](#8-input-touch--button-subsystem)
9. [Userland, SpringBoard & Wallpaper Architecture](#9-userland-springboard--wallpaper-architecture)
10. [Exhaustive Bug & Crash Catalog](#10-exhaustive-bug--crash-catalog)
11. [Daily Toolchain & Operational Commands](#11-daily-toolchain--operational-commands)
12. [Roadmap & Immediate Next Steps](#12-roadmap--immediate-next-steps)

---

## 1. Core Safety Rules & Invariant Guardrails

> [!CAUTION]
> **READ BEFORE TOUCHING HARDWARE OR RUNNING SCRIPTS.** Violating these rules can result in permanent device bricking or stranding the phone in an unrecoverable state.

1. **BROKEN BUTTONS RULE (NEVER RELY ON PHYSICAL BUTTONS):**
   - The physical **Volume Up, Volume Down, and Power buttons** on this test phone are **physically broken / unresponsive**.
   - **NEVER** instruct the user or a test harness to hold key combinations (e.g., `Power + VolDown` or `VolUp + USB`).
   - Every reboot must be programmatic or autonomous via software watchdogs/deadlines.
2. **EPHEMERAL BOOT ONLY (NEVER FLASH `boot` PARTITION):**
   - **NEVER** run `fastboot flash boot ...` targeting physical partition `mmcblk0p35`.
   - **ALWAYS** boot ephemerally via RAM injection:
     ```bash
     fastboot boot out/xnu-boot.img
     ```
   - If the booted kernel panics or deadlocks, the hardware warm-resets straight into TWRP recovery. Flashing a non-working image to `boot` will brick the autonomous recovery loop.
3. **BCB RECOVERY COOKIE MUST REMAIN ARMED AT ALL TIMES:**
   - The Bootloader Control Block (BCB) cookie `boot-recovery\0` (14 bytes) must be permanently armed in the `misc` partition (`mmcblk0p38`):
     ```bash
     printf 'boot-recovery\0' | dd of=/dev/block/bootdevice/by-name/misc bs=14 count=1 conv=notrunc
     ```
   - This ensures that any unexpected hardware watchdog reset, PMIC reset, or `op2_deadline` software trigger reboots directly into TWRP recovery where `adb` is active.
4. **KERNEL COMMAND-LINE LENGTH LIMIT (256 BYTES):**
   - In XNU `xnu-4903.270.47`, `BOOT_LINE_LENGTH` is defined as **256 bytes** (`cmdline[256]`).
   - If the boot command line exceeds 255 bytes, XNU silently truncates it at byte 255 without a trailing null terminator. A truncated parameter (e.g., `metal=2` becoming `met`) will corrupt parameter parsing.
   - The verified working command line is currently 236 bytes:
     ```text
     -v cpus=4 keepsyms=1 op2_deadline=3000 rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 op2fb_client_ui=1 op2_tapms=2500 op2fb_keepon=1 op2_userlog=0 op2fb_hz=60
     ```

---

## 2. Hardware Specifications & Physical Memory Map

### 2.1 Silicon Overview
- **SoC:** Qualcomm Snapdragon 810 v2.1 (`MSM8994`)
- **CPU Topology:**
  - **Little Cluster:** 4x ARM Cortex-A53 (Cores 0..3, MPIDR `0x00`..`0x03`), in-order, 1.5 GHz max.
  - **Big Cluster:** 4x ARM Cortex-A57 (Cores 4..7, MPIDR `0x100`..`0x103`), out-of-order, 2.0 GHz max.
- **GPU:** Qualcomm Adreno 430 (`oxili`), 650 MHz, Tile-Based Immediate Renderer (TBIR).
- **RAM:** 4 GB Dual-Channel LPDDR4 (PoP package at 1600 MHz, 25.6 GB/s bandwidth).
- **Storage:** 64 GB Samsung eMMC 5.0 (HS400 interface, 8-bit bus).
- **Display:** 5.5" JDI IPS-NEO LCD, 1080×1920 (FHD), 60 Hz DSI Command Mode panel via Qualcomm MDP5 MDSS.
- **Touch Digitizer:** Synaptics S3320 capacitive touch sensor over I2C bus 4 (`0x20`).
- **Interrupt Controller:** ARM GICv2 (`qcom,msm-qgic2`), MMIO distributor at `0xf9000000`, CPU interface at `0xf9002000`.
- **Architected Timer:** ARMv8 Generic Timer, clock rate **19,200,000 Hz (19.2 MHz)**.

### 2.2 DRAM Physical Memory Map
From `/proc/iomem` and `live.dts`:
```text
0x00000000 - 0x062fffff : Lower DRAM Bank (~99 MB)
  0x00080000            : Linux / LK load entry point
0x06300000 - 0x07000000 : memory_hole (13 MB, reserved by TrustZone)
0x07000000 - 0x0ca00000 : modem_mem (90 MB, Qualcomm baseband firmware)
0x0ca00000 - 0x0e900000 : peripheral_mem (31 MB, ADSP / WCNSS)
0x0e900000 - 0x10200000 : tzapp_mem (25 MB, TrustZone secure applets / QSEE)
0x10200000 - 0xf7ffffff : Main System DRAM Bank (~3.7 GB contiguous usable memory)
  0x10200000            : XNU Kernel physical load address
  0xac000000 - 0xac100000 : ramoops_mem (1 MB, persistent pstore console buffer)
0xf8000000 - 0xffffffff : MMIO Window (MSM8994 hardware register space)
```

### 2.3 Page Size & Memory Attributes
- **Page Size:** 4 KB page granule (`ARM_PGSHIFT = 12`). 3-level page translation table.
- **Normal Cached DRAM:** Mapped with `VM_WIMG_COPYBACK` (`kIOMapDefaultCache`). Fully cached in L1/L2.
- **Write-Combine DRAM:** Mapped with `VM_WIMG_WCOMB` (`kIOMapWriteCombineCache`). Normal memory, non-cacheable, buffered writes via write-combining buffer. Used for physical framebuffers and display carveout.
- **Device MMIO:** Mapped with `VM_WIMG_IO` (`kIOMapInhibitCache`). Device-nGnRnE memory.
- **Alignment Traps on Device Memory (`bzero` `DC ZVA`):**
  - In ARM64, the `bzero` implementation uses `dc zva, xN` to zero 128-byte cache lines.
  - On ARM Cortex cores, executing `dc zva` on `Device-nGnRnE` memory triggers an architectural Alignment Fault (`ESR 0x96000061`).
  - *Invariant:* **NEVER** map render surfaces or generic memory buffers from physical addresses outside DRAM that resolve to `Device-nGnRnE` memory. All surface memory must be backed by normal DRAM or `IOBufferMemoryDescriptor`.

---

## 3. Boot Chain & Bare-Metal Shim Architecture

### 3.1 Boot Sequence Diagram
```mermaid
flowchart TD
    A["Fastboot Host: 'fastboot boot out/xnu-boot.img'"] --> B["Qualcomm Little Kernel (aboot) in RAM at 0x0F900000"]
    B -->|"Validates Android boot.img v0 header"| C["Unpacks Kernel & Ramdisk to RAM"]
    C -->|"Kernel at 0x00080000"| D["Shim Entry (shim/start.S)"]
    C -->|"Ramdisk at 0x01000000"| E["OP2XNU01 Payload in RAM"]
    D --> F["shim/main.c: Disable Hardware Watchdog"]
    D --> G["shim/loader.c: Relocate XNU Segments to PA 0x10200000"]
    D --> H["Inject ADT (Apple Device Tree) & boot_args at PA 0x10C59000"]
    D --> I["Jumps to XNU Entry (LowResetVectorBase @ 0x10294000)"]
    I --> J["XNU: arm_vm_init, pmap bootstrap, GICv2 init"]
    J --> K["XNU: Mounts HFS+ Root from eMMC mmcblk0p43"]
    K --> L["PID 1: /sbin/launchd -> backboardd -> SpringBoard"]
```

### 3.2 The Boot Shim Components (`shim/`)
- [`shim/start.S`](../shim/start.S): Drops from bootloader EL to EL1 (if needed), configures stack pointer, jumps to `main()`.
- [`shim/main.c`](../shim/main.c): Disables Qualcomm PMIC hardware watchdog registers, queries LK boot tags, parses DTB at `0x2C`.
- [`shim/loader.c`](../shim/loader.c): Reads `OP2XNU01` container format from ramdisk, relocates segments into DRAM (`0x10200000`), sets up `boot_args` structure (PA `0x102ea000`), and passes control to XNU.
- [`shim/build.py`](../shim/build.py): Compiles shim with `clang -target aarch64-none-elf`, links via `ld.lld`, injects Little Kernel DTB offset at `0x2C`, appends stock 7-DTB blob (`dtbs.blob`), and generates `out/xnu-boot.img` with `mkbootimg.py`.

---

## 4. SMP & Multicore CPU Management

### 4.1 Topology & Current Configuration
- **Active Cores:** 4x Cortex-A53 Little cluster cores (CPU 0, 1, 2, 3) running in quad-core SMP (`cpus=4`).
- **Boot Core:** CPU 0 boots directly from the shim.
- **Secondary Cores (CPU 1..3):** Brought online dynamically by XNU via `OP2CPU::start()` in [`xnu-4903.270.47/iokit/Kernel/arm/OP2CPU.cpp`](../xnu/xnu-4903.270.47/iokit/Kernel/arm/OP2CPU.cpp).

### 4.2 Core Release Mechanism
1. Qualcomm TrustZone does **not** implement standard PSCI `CPU_ON` on MSM8994 (returns `0xffffffff`).
2. Secondary cores are started via Qualcomm SCM (Secure Channel Manager) TrustZone calls:
   - SCM Call: Set boot address for secondary core to `op2_secondary_entry` (PA `0x10496000`).
   - SCM Call: Issue ACC unclamp sequence (power clamp release, GDHS close, core reset de-assert, LDO configure).
   - Issue `SEV` (Send Event) broadcast to wake cores from WFE/WFI loop.
3. Cores enter `op2_secondary_entry` in [`osfmk/arm64/start.s`](../xnu/xnu-4903.270.47/osfmk/arm64/start.s), initialize MMU using master translation table (`ttep 0x10c41000`), attach to per-CPU data, and signal readiness to master core.
4. Quad-core SMP cuts boot time from 30+ minutes on 1 core down to ~4-5 minutes.

---

## 5. Storage Architecture & eMMC Driver

### 5.1 Partition Layout (`/dev/block/bootdevice/by-name/`)
| Partition Name | Block Device | Size | Filesystem | Role in iOS Bring-Up |
| :--- | :--- | :--- | :--- | :--- |
| `boot` | `mmcblk0p35` | 64 MB | Android Boot v0 | **DO NOT TOUCH** (Stock kernel/TWRP boot fallback) |
| `recovery` | `mmcblk0p36` | 64 MB | Android Boot v0 | TWRP Recovery 3.0+ |
| `misc` | `mmcblk0p38` | 1 MB | Raw | Stores BCB cookie (`boot-recovery\0`) |
| `system` | `mmcblk0p42` | 3.0 GB | HFS+ (Read-Only) | Secondary staging / backup root |
| `userdata` | `mmcblk0p43` | ~53 GB | HFSX (Case-Sensitive) | **iOS 12.5.7 Live Root Filesystem (`emmcroot=userdata`)** |

### 5.2 Driver Implementation ([`op2_emmc.c`](../xnu/xnu-4903.270.47/bsd/dev/op2_emmc.c))
- Custom XNU block/character device driver (`emmcroot0`, major 22/23).
- Uses Qualcomm SDHC controller at `0xf9824900` via polled PIO (busy-waiting on `SDHCI_INT_STATUS`).
- **Direct Aligned Transfer Optimization (Milestone 26):**
  - When `chunk == EMMC_SECTOR_SIZE` and destination pointer is 4-byte aligned, `sdhc_rd32(SDHCI_BUFFER)` reads directly into the caller's memory address, bypassing intermediate sector buffer copies.
- **Reliability Record:** Over 2,019,112 block reads and 1,450,000 writes completed with **0 failures** during the 50-minute soak test.

### 5.3 Rootfs Staging & Flashing
- Tool: [`tools/mkop2root.py`](../tools/mkop2root.py) mounts official DMG (`out/038-87140-050.dmg`), creates HFSX volume, copies system files, and applies iOS bring-up patches.
- Flasher: [`tools/flash_userdata.py`](../tools/flash_userdata.py) streams compressed DMG image directly into `mmcblk0p43` over ADB port forward using `busybox nc -l -p 12345 | gunzip -c | dd of=/dev/block/bootdevice/by-name/userdata` (~30 MB/s transfer rate, ~3.5 min total). Automatically re-arms BCB cookie after streaming.

---

## 6. Display Pipeline & High-Speed Frame Downsampler

### 6.1 Hardware Specifications
- **Display Controller:** Qualcomm Mobile Display Subsystem (MDSS) Mobile Display Processor 5 (MDP5).
- **Physical Panel:** JDI 1080×1920 24-bit RGB command-mode panel.
- **Continuous Framebuffer Base:** `0x03400000` (MDSS VIG0 pipe).
- **Row Bytes:** 4352 bytes ($1080 \times 4 = 4320$, padded to 4352 for 128-byte hardware DMA alignment).
- **Format:** 32bpp BGRA (`OP2_FB_FORMAT_BGRA`).

### 6.2 Display Geometry & Downsampling Architecture
- **UIKit Logical Resolution:** On iPhone 6 Plus (`N56AP`), UIKit and CoreAnimation render at **3x Retina ($1242 \times 2208$)**.
- `AppleCLCD::getDisplaySize` reports $1242 \times 2208$ to `backboardd`, causing SpringBoard to allocate $1242 \times 2208$ render surfaces.
- **The 54-Block 20-Pixel Linear Downsampler ([`OP2Framebuffer.cpp`](../xnu/xnu-4903.270.47/iokit/Kernel/arm/OP2Framebuffer.cpp#L928-L985)):**
  - Converts $1242 \times 2208 \to 1080 \times 1920$ on every frame.
  - Mathematical ratio: $1080 = 54 \times 20$ output pixels, $1242 = 54 \times 23$ input pixels ($23 / 20 = 1.15$).
  - Operates strictly with contiguous 64-bit pair registers (`ldp`/`stp`):
    - Sub-block 1: Pixels 0..5 (3x 64-bit pair stores) + pixel 6 (32-bit). Pixel 7 skipped.
    - Sub-block 2: Pixel 7 (32-bit). Pixels 8..13 (3x 64-bit pair stores). Pixel 15 skipped.
    - Sub-block 3: Pixels 14..19 (3x 64-bit pair stores). Pixel 22 skipped.
  - **Latency:** **6,035 $\mu\text{s}$ (6.0 ms)** per frame (down from 13.7 ms in Milestone 25), allowing continuous 60 fps presentation with zero jitter.
- **Direct Hardware Scanout:** When a surface matches native panel geometry ($1080 \times 1920$), `AppleCLCD::present` reprograms MDP5 VIG0 `PIPE_SRC0_ADDR` directly to the surface's physical address, achieving **0 ms zero-copy scanout**.

---

## 7. Graphics & Metal Acceleration Architecture

### 7.1 Architecture Overview
- **Adreno 430 GPU Hardware:** Driven by `OP2GPU` in [`xnu-4903.270.47/iokit/Kernel/arm/OP2GPU.cpp`](../xnu/xnu-4903.270.47/iokit/Kernel/arm/OP2GPU.cpp).
  - Handles Qualcomm RPM SMD power voting, oxili GDSC power domain uncollapse, Adreno 430 CP microcode loading, and ringbuffer submission.
- **Kernel User Client:** `OP2GPUUserClient` exposes `msm_drm` ABI, mapping contiguous DDR GEM memory and dispatching EL0 PM4 indirect command buffers.
- **Userland Driver:** [`userland/OP2Metal/OP2Metal.bundle`](../userland/OP2Metal/OP2Metal.m) implements Apple's Metal runtime interfaces (`MTLDevice`, `MTLCommandQueue`, `MTLCommandBuffer`, `MTLRenderCommandEncoder`, `MTLTexture`).
  - Successfully satisfies `MTLCreateSystemDefaultDevice()`, parses `default.metallib` (62 shaders), and manages `CAMetalLayer` textures.

### 7.2 Current Operational Mode: CPU Software Rasterization (`metal=0`)
- **Why `metal=0` is default for UI stability:**
  - When `metal=2` was enabled, CoreAnimation detected `OP2Metal` and routed drawing commands to it. Because `OP2Metal.m` does not yet translate all Apple CoreAnimation rasterization states into Adreno PM4 drawing packets, CoreAnimation produced blank frames (`nz=0`).
  - With `metal=0` (omitting `metal=` from boot args), `AGXAccelerator` is omitted from the IORegistry. CoreAnimation gracefully falls back to Apple's native in-tree CPU software rasterizer, drawing clock digits, dates, icons, drop shadows, and wallpapers directly into surface memory.
- **Future GPU Step:** Implement hardware draw call packet generation in `OP2Metal.m` or hybridize it to fall back to CPU software rasterization for unsupported primitives while hardware-accelerating surface blits and composition.

---

## 8. Input, Touch & Button Subsystem

### 8.1 Digitizer Touch Controller ([`OP2Digitizer.cpp`](../xnu/xnu-4903.270.47/iokit/Kernel/arm/OP2Digitizer.cpp))
- Hardware: Synaptics S3320 over I2C bus 4 (`0xf9927000`, slave address `0x20`).
- Reports multi-touch coordinates mapped directly to display space ($1080 \times 1920$).
- Dispatches `IOHIDEvent` packets to `backboardd` via Mach message queues.

### 8.2 Dedicated Consumer Buttons Service (`OP2Buttons`)
- Hardware: Capacitive Home button / fingerprint bezel on OnePlus 2.
- Implements `IOHIDEventService` publishing Consumer Control usage (Page `0x0C`, Usage `0x40`).
- Wakes the Lock Screen and triggers Home Screen transitions in `SpringBoard`.
- **Mach Port Exclusivity Invariant:**
  - In `OP2ButtonsUserClient::registerNotificationPort`, only `backboardd` is permitted to bind notification ports. If `SpringBoard` attempts to register, the call is rejected. This prevents SpringBoard respawns from wiping backboardd's port and dropping Home button events.

---

## 9. Userland, SpringBoard & Wallpaper Architecture

### 9.1 Apple CoreProfile Bitmap (`.cpbitmap`) Specification
Through binary reverse engineering of `AppSupport.dylib` (`CPBitmapCreateImagesFromData` at `0x1836f76c0`), the exact format required by iOS 12.5.7 was proven:
```text
Offset 0x00000000 .. 0x019A1000 : 2592 * 2592 * 4 bytes BGRA pixel data (26,873,856 bytes)
Offset 0x019A1000 .. 0x019A101C : 28-Byte Apple CPBitmap Binary Trailer:
  Word 0 (uint32) : Colorspace length = 0
  Word 1 (uint32) : Info dictionary length = 0
  Word 2 (uint32) : Width = 2592
  Word 3 (uint32) : Height = 2592
  Word 4 (uint32) : Pixel format = 1 (BGRA 32bpp)
  Word 5 (uint32) : Image count = 1
  Word 6 (uint32) : Magic = 0xdcb543a2 (Bytes: a2 43 b5 dc)
Total File Size  : Exactly 26,873,884 bytes
```

### 9.2 Critical Files in `~/Library/SpringBoard/`
All files must have `0644` permissions and owner `501:501` (`mobile:mobile`):
- `LockBackground.cpbitmap`: 26,873,884 bytes
- `HomeBackground.cpbitmap`: 26,873,884 bytes
- `OriginalLockBackground.cpbitmap`: 26,873,884 bytes
- `OriginalHomeBackground.cpbitmap`: 26,873,884 bytes
- `LockWallpaperOptions.plist` & `HomeWallpaperOptions.plist`: Plists defining `wallpaperScale = 1.0`, `cropRect = {{0, 0}, {2592, 2592}}`, `name = WWDC2018`.
- `DesiredIconState.plist` & `IconState.plist`: Defines dock icons (`com.apple.mobilephone`, `com.apple.mobilesafari`, `com.apple.MobileSMS`, `com.apple.Music`) and page layout.
- `TodayViewArchive.plist`: Widgets configuration.
- `SBLastRestoreIdentifier`: String UUID.

---

## 10. Exhaustive Bug & Crash Catalog

| Bug / Crash Signature | Root Cause | Structural Fix Applied | Verification |
| :--- | :--- | :--- | :--- |
| **Crash A: NSDictionary nil `objects[4]`** | `-[NSUserDefaults ndo_registerDefaults]` calls `MGCopyAnswer(@"SerialNumber")` $\to$ nil (no hardware syscfg). | Injected synthetic device serials in [`tools/mkadt.py`](../tools/mkadt.py) (`serial-number`, `mlb-serial-number`, `region-info`, `model-number`). | `ndoagent` SIGABRT eliminated (17 $\to$ 0). |
| **Crash B: `routingContextUID` assertion in SpringBoard** | ControlCenter loaded `nowplaying` audio module. `MRAVOutputContextGetSharedAudioPresentationContext()` returned `nil` on audio-less hardware $\to$ passed to `setRoutingContextUID:` $\to$ assertion throw. | Removed `com.apple.mediaremote.controlcenter.nowplaying` from `ModuleWhitelist.plist` in [`tools/mkop2root.py`](../tools/mkop2root.py). | SpringBoard runs 50+ min with 0 SIGABRTs. |
| **Alignment Fault in `bzero` (`ESR 0x96000061`)** | Carveout at `0x03c00000` mapped with `kIOMapDefaultCache` defaulted to `Device-nGnRnE`. `bzero` executed `dc zva` on Device memory $\to$ alignment fault at 793s. | Restricted carveout strictly to 1080x1920 panel buffers. 1242x2208 surfaces allocate from cached system DRAM via `IOBufferMemoryDescriptor` in [`OP2Surface.cpp`](../xnu/xnu-4903.270.47/iokit/Kernel/arm/OP2Surface.cpp#L806). | Zero kernel aborts across multiple 50-minute soaks. |
| **`libGFXShared` Segfault** | `_gfxInitializeLibrary` queried IORegistry for `IOGLESBundleName`. Kernel only published macOS desktop `IOGL*` properties $\to$ returned `NULL` $\to$ `CFStringGetLength(NULL)` faulted. | Published `IOGLESBundleName = "OP2Metal"` and `IOGLESMetalBundleName = "OP2Metal"` in [`OP2Framebuffer.cpp`](../xnu/xnu-4903.270.47/iokit/Kernel/arm/OP2Framebuffer.cpp). | Segfault resolved. |
| **`itunescloudd` SQLite Crash** | `HSCloudClientGetPreferredVideoQuality` attempted to query `MediaLibrary.sqlitedb`, which did not exist on fresh rootfs $\to$ `errno 2` $\to$ SIGABRT. | Seeded valid SQLite database schema in `/private/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb`. | `itunescloudd` aborts eliminated. |
| **Baseband `libARI` NULL Release** | Without cellular modem hardware, `libARI` releases `NULL` ~22x per run. Stock `CFRelease` traps on `NULL`. | Surgical binary patch at `0xdb0d88` in dyld shared cache: `cbz x0, +8` to return early on NULL. | Cellular respawn loop prevented. |
| **SpringBoard Setup & Lockdown Traps** | SpringBoard halted on activation / setup mode checks without Apple secure element. | Binary patches in SpringBoard: `-[SBSetupManager isInSetupMode] -> return NO`, `-[SBLockdownManager lockdownState] -> return 2 (Activated)`. | Setup assistant bypassed. |
| **Jetsam Daemon Kill Epidemic** | `n56ap` profile enforces legacy 36 MB footprint limit on daemons (`assistivetouchd`), causing fatal kills on 4 GB device. | Added 256 MB task memory floor in [`osfmk/kern/task.c`](../xnu/xnu-4903.270.47/osfmk/kern/task.c). | Daemons remain permanently resident. |

---

## 11. Daily Toolchain & Operational Commands

### 11.1 Building the XNU Kernel
```bash
# Clean or incremental build of XNU (output: xnu/xnu-4903.270.47/BUILD/obj/DEVELOPMENT_ARM64_MSM8994/kernel.development.msm8994)
./tools/build-xnu.sh
```

### 11.2 Generating ADT, Payload & Boot Image
```bash
# 1. Generate Apple Device Tree (ADT)
python3 tools/mkadt.py out/op2-hardware.adt

# 2. Package XNU kernel and ADT into OP2XNU01 binary container
python3 tools/mkpayload.py \
  xnu/xnu-4903.270.47/BUILD/obj/DEVELOPMENT_ARM64_MSM8994/kernel.development.msm8994 \
  out/op2-hardware.adt \
  out/op2-hardware-payload.bin \
  --cmdline "-v cpus=4 keepsyms=1 op2_deadline=3000 rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 op2fb_client_ui=1 op2_tapms=2500 op2fb_keepon=1 op2_userlog=0 op2fb_hz=60"

# 3. Assemble Android boot.img v0 with bare-metal shim
python3 shim/build.py --payload out/op2-hardware-payload.bin --output out/xnu-boot.img
```

### 11.3 Staging & Flashing Userdata Rootfs
```bash
# Stage changes into DMG
sudo python3 tools/mkop2root.py out/038-87140-050.dmg out/ios12-userdata-fixA.dmg 6

# Stream DMG directly to device over netcat (device must be in TWRP recovery)
python3 tools/flash_userdata.py out/ios12-userdata-fixA.dmg
```

### 11.4 Running Hardware Soak Test
```bash
# Boots device, tracks execution for 50 minutes, harvests ramoops upon reset, re-arms BCB
python3 tools/run-phase8.py
```

### 11.5 Manual Diagnostic Inspection (in TWRP Recovery)
```bash
# Read harvested ramoops log
cat logs/hardware-milestone26-wallpaper-smooth-verified.log | grep -E "OP2 FB|OP2 eMMC|panic\("

# Pull live pstore directly from phone
adb pull /sys/fs/pstore/console-ramoops-0 /tmp/ramoops.log

# Verify BCB cookie status on phone
adb shell "dd if=/dev/block/bootdevice/by-name/misc bs=1 count=14 2>/dev/null"
```

---

## 12. Roadmap & Immediate Next Steps

For the incoming engineer / agent taking over:

1. **Adreno 430 Metal Pipeline State Completion:**
   - In [`userland/OP2Metal/OP2Metal.m`](../userland/OP2Metal/OP2Metal.m):
     - The device is currently running in `metal=0` (Apple CPU software rasterizer fallback) because `OP2Metal.m` lacks complete Adreno PM4 packet generation for complex CoreAnimation blend states.
     - Complete the PM4 packet encoder in `OP2MetalCommandBuffer` to emit Adreno draw packets into GPU shared memory (`OP2GPUUserClient`), or build a hybrid architecture where standard 2D layer blits are GPU-accelerated while unsupported shader stages safely pass through to CPU composition.
2. **Big Cluster (Cortex-A57) SMP Bring-Up:**
   - Currently, 4x Cortex-A53 cores (CPU 0..3) are running.
   - Investigate enabling the 4x Cortex-A57 cores (CPU 4..7) via `qcom,8994-arm-cortex-acc` sequences in `OP2CPU.cpp` for true octa-core execution or big.LITTLE core migration.
3. **First-Party App Launch Validation:**
   - Test launching Safari, Preferences (Settings), Camera, Photos, and Calculator from the Home Screen.
   - Track exit-ring codes and Mach exception ports in `sleh.c` if apps encounter missing entitlement checks or camera sensor MMIO stalls.
4. **Power Management & Display Sleep:**
   - Currently, `op2fb_keepon=1` prevents the screen from entering deep sleep.
   - Wire up MDP5 power collapse / DSI panel sleep transitions with proper backlight PWM control on wake.
