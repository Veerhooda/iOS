# XNU Port Hacks & Technical Debt Tracker (OnePlus 2 / MSM8994)

This document tracks all temporary shims, workarounds, and non-upstream modifications introduced into the Darwin/XNU kernel (`xnu-4903.270.47`) and bootloader shim (`shim/`) during bring-up on the OnePlus 2 (Snapdragon 810 / MSM8994).

Each entry describes:
1. **The Hack / Modification** (files affected)
2. **Why it was necessary** (early-boot wall or dependency)
3. **Risks & Behavioral Differences** (impact on userland and system security)
4. **Condition for Removal / Replacement** (permanent path forward)

---

## 1. Built-in `libpthread-330.250.2` Integration

- **Files:**  
  `bsd/pthread/op2_libpthread/`  
  `bsd/pthread/op2_pthread_compat.h`  
  `bsd/pthread/op2_pthread_init.c`  
  `bsd/pthread/op2_pthread_support.c`  
  `bsd/pthread/op2_pthread_synch.c`  
  `bsd/pthread/pthread_shims.c`  
  `bsd/conf/Makefile.template`  
  `bsd/conf/files`
- **Why it was necessary:**  
  On standard Apple hardware, the POSIX thread kernel subsystem is delivered as a closed-source Mach-O kext (`com.apple.kec.pthread`). In open-source XNU, `bsd_init()` fails with `panic("pthread kernel extension not loaded")` if the kext registration callback `pthread_init()` has not occurred. We imported open-source `libpthread-330.250.2` and linked it directly into the kernel image. Because `libpthread-330.250.2` lacked ARM64 user-thread context definitions in `kern_support.c`, the ARM64 register setup functions (`thread_set_wq_state64`, `bsdthread_create`) were backported from `libpthread-514.100.2`.
- **Risks & Userland Impact:**  
  Mixed version codebase: kernel pthread structures come from 330.250.2 (iOS 12/macOS 10.14 era), while ARM64 thread-state manipulation references 514.100.2. Once real multi-threaded userland processes spawn (e.g. `launchd`, `dyld`, `libdispatch`), subtle layout or ABI mismatches in `workq` or `psynch` structures could trigger kernel memory corruption or thread deadlocks.
- **Condition for Removal:**  
  Retain built-in pthread, but audit all `arm_thread_state64_t` offsets against the target iOS 12.5.7 userland `libsystem_pthread.dylib` ABI, or extract and link the exact matching `pthread.kext` binary from the target iOS 12.5.7 kernelcache.

---

## 2. Corecrypto Null-Pointer Defensive Guards

- **Files:**  
  `libkern/crypto/corecrypto_aes.c`  
  `libkern/crypto/corecrypto_sha1.c`  
  `libkern/crypto/corecrypto_sha2.c`  
  `libkern/crypto/corecrypto_md5.c`
- **Why it was necessary:**  
  On macOS/iOS, cryptographic primitives are initialized by `corecrypto.kext`. In our standalone monolithic build, early network initialization (`bsd_init` -> `socketinit` -> `domaininit` -> `tcp_init` -> `tcp_tfo_init`) calls `aes_encrypt_key128()`. Because `g_crypto_funcs` was `NULL`, the kernel suffered an immediate Data Abort at `g_crypto_funcs + 0x78`. Null guards were added returning `aes_error` or no-op.
- **Risks & Userland Impact:**  
  Any kernel subsystem relying on AES, SHA-1, SHA-256, or MD5 via `corecrypto_*` will receive an error return code rather than actual encrypted or hashed bytes. While TCP TFO degrades gracefully, IPsec, keychain, disk encryption, and cryptographic signatures will fail.
- **Condition for Removal:**  
  Properly populate `g_crypto_funcs` by embedding Apple's open-source `corecrypto` library implementation or registering early software crypto function tables before `domaininit()` is called.

---

## 3. In-Kernel Software HMAC-SHA256 PRNG

- **Files:**  
  `osfmk/prng/prng_random.c`  
  `osfmk/prng/random.h`
- **Why it was necessary:**  
  During Mach bootstrap, `commpage_populate()` requires random entropy to initialize the commpage ASLR canaries, user pointers, and stack guards. Open-source XNU expects `AppleARMPE` to register hardware random callbacks via `register_and_init_prng()`. Without this, `read_random()` spins or asserts. We registered an in-kernel NIST HMAC-SHA256 DRBG seeded with `ml_get_timebase()` and early random.
- **Risks & Userland Impact:**  
  The PRNG has **no physical entropy source**. It is seeded solely by the CPU virtual timer tick (`ml_get_timebase()`) and a static string (`"xnu msm8994 kernel random"`). ASLR base addresses, UUIDs, stack canaries, and `/dev/urandom` in userland are predictable across reboots.
- **Condition for Removal:**  
  Implement an entropy feeder using Qualcomm's hardware Random Number Generator (`0xf9bff000`, `qcom,msm-rng` in the OnePlus 2 device tree) or read random seeds passed from LK bootloader in the device tree `/chosen/rng-seed`.

---

## 4. `IOBSD` 30-Second Stall Bypass

- **Files:**  
  `iokit/bsddev/IOKitBSDInit.cpp`
- **Why it was necessary:**  
  In `IOFindBSDRoot()`, XNU executes `IOService::waitForMatchingService(matching, 30ULL * kSecondScale)` waiting for a published `IOBSD` resource (signaling filesystem drivers are ready). Because no external filesystem kexts are loaded dynamically in early bring-up, this unconditionally caused a 30-second hang before falling back to `!BSD`. Bypassed to advance boot instantly.
- **Risks & Userland Impact:**  
  Hardcoded to `#if defined(ARM64_BOARD_CONFIG_MSM8994)`. If real root storage relies on delayed filesystem driver matching, it could race against root mounting.
- **Condition for Removal:**  
  Gate the bypass on a boot-argument (e.g. `skip_iobsd_wait=1` or `fastboot`) instead of a hardcoded `#ifdef`.

---

## 5. Dual-Core SMP Bring-Up & Immediate `IORTC` Publication

- **Files:**  
  `iokit/Kernel/arm/OP2PlatformExpert.cpp`  
  `iokit/Kernel/arm/OP2CPU.cpp`  
  `osfmk/arm/arm_init.c`  
  `osfmk/arm64/start.s`
- **Why it was necessary:**  
  - *Initial Bring-Up:* MSM8994 has 8 cores (4x Cortex-A57 + 4x Cortex-A53). Bringing up secondary cores originally failed because Qualcomm leaves secondary cores parked in a low-power WFI loop. The system originally defaulted to `ml_init_max_cpus(1)`.
  - *Dual-Core Resolution (Milestone 9):* Enabled `cpus=2` by implementing mailbox spin-table synchronization (`0x00080000` / `0x00080008`) and issuing ARM PSCI SMC calls (`CPU_ON = 0xC4000003`) to release CPU 1. CPU 1 enters `arm_init_secondary` in `start.s`, configures its MMU and GICv2 interface, and joins the Mach scheduler.
  - *`publishResource("IORTC", kOSBooleanTrue)`:* XNU's `bsd_init` -> `inittodr` calls `IOService::waitForService(IOService::resourceMatching("IORTC"))`. Without real RTC hardware publishing this service, the kernel halts forever waiting for the clock. Publishing it unconditionally unblocks `inittodr`.
- **Risks & Userland Impact:**  
  The system currently runs dual-core on the Little cluster (`CPU 0 + CPU 1`). Cores 2–7 remain offline. Wall clock time initializes to Unix epoch or build timestamp until the PM8994 SPMI RTC driver is implemented.
- **Condition for Removal:**  
  - Scale SMP to 4 cores on the Little cluster (`CPU 0-3`) and subsequently to the Big cluster.
  - Write an SPMI RTC driver matching Qualcomm PM8994 RTC (`0x02005000`) to back `IORTC` with real wall-clock hardware registers.

---

## 6. Root Wait Panic Hook

- **Files:**  
  `iokit/bsddev/IOKitBSDInit.cpp`
- **Why it was necessary:**  
  When booting without root media, `IOFindBSDRoot()` loops indefinitely: `do { waitForService(); } while (!service);`. On hardware with watchdog disabled, this causes the phone to park at the boot logo indefinitely, requiring battery pulls. An explicit panic terminates execution if no root device matches, triggering automated warm reset back to TWRP.
- **Risks & Userland Impact:**  
  The kernel will deliberately panic instead of waiting indefinitely if root media fails to match immediately.
- **Condition for Removal:**  
  Once secondary storage drivers (eMMC) are completely stable, restore the normal `waitForService` timeout loop.

---

## 7. Qualcomm Watchdog Disabled in Bootloader Shim

- **Files:**  
  `shim/main.c`
- **Why it was necessary:**  
  The APPS SoC hardware watchdog at `0xf9017000` was tested with a deliberate hang. When the watchdog counter expired, rather than triggering a clean warm reset, the phone completely dropped off USB and required a physical battery disconnect/reconnect to recover (likely due to PMIC emergency rail latching). The watchdog was disabled in `shim/main.c` to prevent unrecoverable device lockouts.
- **Risks & Userland Impact:**  
  If the kernel hangs in a tight spinlock or fault before panic handling, the phone will stay powered on and warm without automatically rebooting.
- **Condition for Removal:**  
  Investigate IMEM download-mode cookies (`msm_poweroff.c`) and configure TrustZone SCM calls to ensure hardware bites trigger a clean reboot into TWRP.

---

## 8. Gated PID 1 Syscall Breadcrumbs

- **Files:**  
  `osfmk/arm64/sleh.c`
- **Why it was necessary:**  
  Provides immediate kernel-side breadcrumbs (`PID 1 syscall: %lld`) as PID 1 invokes syscalls, ensuring unmistakable log evidence of userland execution even if `/dev/console` write blocks or buffers in the tty layer.
- **Risks & Userland Impact:**  
  Gated behind the boot-argument `trace_syscalls=1`. When absent (default), executes zero prints on the syscall fast path.
- **Condition for Removal:**  
  Remove once userland console I/O and process lifecycle are completely established and reliable.

---

## 9. Built-in Apple HFS+ Driver & macOS 10.14 Compatibility Wrappers

- **Files:**  
  `bsd/op2_hfs/` (core/ and hfs_encodings/)  
  `bsd/op2_hfs/op2_hfs_compat.h`  
  `bsd/op2_hfs/op2_hfs_iokit.c`
- **Why it was necessary:**  
  `xnu-4903` lacks the in-kernel HFS filesystem driver (Apple removed it from the open-source tree in macOS 10.14 and distributed it as a closed-source kext). We staged Apple's open-source `hfs-627.100.6` directly into `bsd/op2_hfs/` and linked it built-in. Compatibility shims bridge macOS 13 APIs back to `xnu-4903` KPIs.
- **Risks & Userland Impact:**  
  Filesystem metadata operates in unencrypted HFS+ mode (AppleKeyStore is bypassed).
- **Condition for Removal:**  
  Retain for HFS+ root filesystem support until APFS is operational.

---

## 10. ARM GICv2 Interrupt Controller & Generic Virtual Timer

- **Files:**  
  `iokit/Kernel/arm/OP2InterruptController.cpp`  
  `osfmk/arm64/sleh.c`  
  `pexpert/arm/pe_identify_machine.c`
- **Why it was necessary:**  
  Standard Apple platforms use proprietary interrupt and timer hardware arriving as FIQs. MSM8994 routes interrupts through ARM GICv2 (`arm,gic-400`) and uses the ARM Generic Virtual Timer (`CNTV_CTL_EL0`, `CNTV_TVAL_EL0`). Implemented dynamic GIC distributor (`0xf9000000`) and CPU interface (`0xf9002000`) mapping with 10 ms periodic timer dispatch.
- **Risks & Userland Impact:**  
  Hardware interrupts operate through MMIO registers instead of system registers (`ICC_*`).
- **Condition for Removal:**  
  Permanent architectural driver for MSM8994 hardware.

---

## 11. In-Kernel eMMC 5.0 / SDHCI Block Device Driver

- **Files:**  
  `shim/sdhci.c`  
  `bsd/dev/op2_emmc.c`
- **Why it was necessary:**  
  Little Kernel leaves the Qualcomm SDCC 5.0 controller in legacy MCI mode operating at 200 MHz with DLL locked. The shim resets the core, transitions to Host Controller mode, performs JEDEC `CMD0` and `CMD1` voltage negotiation, and initializes 25 MHz bus speed. In-kernel `op2_emmc.c` provides polled block read/write operations with CMD12 abort handling and 3x retries.
- **Risks & Userland Impact:**  
  Block I/O uses synchronous polled PIO through `SDHCI_BUFFER` rather than asynchronous DMA/ADMA2.
- **Condition for Removal:**  
  Upgrade to SDHCI ADMA2 (scatter-gather DMA) with interrupt completion (`SPI 123` / `INTID 155`) for enhanced throughput.

---

## 12. Jetsam 256MB Task Footprint Limit Floor

- **Files:**  
  `osfmk/kern/task.c`
- **Why it was necessary:**  
  Legacy `n56ap` (iPhone 6 Plus) device profiles assign restrictive 36 MB footprint limits to daemons like `assistivetouchd` and `quicklookd`. On the OnePlus 2 (4 GB physical DRAM), minor memory allocations caused fatal out-of-memory jetsam kills. Clamping task memory limits to a 256 MB minimum floor prevents premature daemon termination.
- **Risks & Userland Impact:**  
  Userland daemons are allowed to consume up to 256 MB before being targeted by `memorystatus`. Given 2.4 GB of managed kernel memory, this poses no memory starvation risk.
- **Condition for Removal:**  
  Inject a custom device profile plist into `/System/Library/CoreServices/` defining modern memory tiers for the device.

---

## 13. Display Zero-Fill Frame Preservation & `op2fb_keepon` Power Veto

- **Files:**  
  `iokit/Kernel/arm/OP2Framebuffer.cpp`  
  `iokit/Kernel/IOPMrootDomain.cpp`
- **Why it was necessary:**  
  When `backboardd` switches display modes or transitions between apps, it submits blank composite frames (e.g. Surface 5). In `AppleCLCD::present`, copying blank data to the continuous splash buffer caused the physical display to go black. The driver freezes the existing non-zero frame in physical panel memory (`fPixels`) when receiving empty composite frames, and vetoes display sleep requests (`op2fb_keepon=1`).
- **Risks & Userland Impact:**  
  If userland intentionally presents a pitch-black frame, the driver may retain the previous non-zero frame until non-zero content is submitted or a timeout expires.
- **Condition for Removal:**  
  Implement full hardware Qualcomm MDSS MDP5 pipeline registers to let the hardware display processor manage layer blending natively.

---

## 14. Dedicated `OP2Buttons` Consumer HID Service

- **Files:**  
  `iokit/Kernel/arm/OP2Digitizer.cpp`
- **Why it was necessary:**  
  In iOS 12, `backboardd` rejects Home button keycodes dispatched from digitizer devices. Creating an independent `OP2Buttons` driver publishing Consumer/ConsumerControl HID usage (Page `0x0C`, Usage `0x40`) binds directly to SpringBoard's Mach notification port, enabling reliable wake and navigation.
- **Risks & Userland Impact:**  
  Synthetic button events bypass the standard IOHIDEventSystem routing and communicate directly with SpringBoard.
- **Condition for Removal:**  
  Integrate hardware button GPIO interrupts directly into the Consumer HID event queue.

---

## 15. Dynamic Userland Surface Routing & 1242×2208 Retina Downsampling

- **Files:**  
  `iokit/Kernel/arm/OP2Framebuffer.cpp`  
  `iokit/Kernel/arm/OP2Surface.cpp`
- **Why it was necessary:**  
  SpringBoard renders UI for the iPhone 6 Plus profile at 1242×2208 Retina resolution, dynamically alternating across userland surfaces (Surfaces 2, 9, 12, 15, 18, 21). The driver automatically scans surface descriptors up to ID 63, bilinearly downsampling Retina surfaces to native 1080×1920 BGR888 panel scanout memory.
- **Risks & Userland Impact:**  
  Software CPU bilinear downsampling consumes CPU cycles on Core 0.
- **Condition for Removal:**  
  Configure Qualcomm MDP5 hardware downscaler pipes to perform hardware-accelerated downsampling directly to DSI.

---

## 16. Code Signing & AMFI Validation Bypasses

- **Files:**  
  `bsd/kern/kern_exec.c`  
  `bsd/kern/kern_cs.c`  
  `bsd/kern/op2_amfi.c`
- **Why it was necessary:**  
  Without Apple's closed-source `AppleMobileFileIntegrity.kext` (AMFI) and hardware Secure Enclave coprocessor (SEP), standard iOS binary validation fails. Boot arguments `cs_enforcement_disable=1` and `launchd_unsecure_cache=1`, combined with kernel AMFI policy hooks, allow unmodified iOS 12 userland binaries and shared caches to execute without signature panics.
- **Risks & Userland Impact:**  
  Binary code-signing validation is disabled. Any binary or dynamic library can execute without valid Apple signatures.
- **Condition for Removal:**  
  Permanent requirement for running stock iOS userland on non-Apple hardware without an Apple Secure Enclave.
