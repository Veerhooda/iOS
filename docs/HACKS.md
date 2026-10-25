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

## 5. `ml_init_max_cpus(1)` and Immediate `IORTC` Publication

- **Files:**  
  `iokit/Kernel/arm/OP2PlatformExpert.cpp`
- **Why it was necessary:**  
  - `ml_init_max_cpus(1)`: MSM8994 has 8 cores (4x Cortex-A57 + 4x Cortex-A53). Bringing up secondary cores requires Qualcomm ACC (Application Clock Controller) clock/voltage regulation and CPU release sequence (`enable-method = "qcom,8994-arm-cortex-acc"` in device tree; no PSCI). Restricting to CPU 0 avoids secondary core panics before SMP brings up CPU nubs.
  - `publishResource("IORTC", kOSBooleanTrue)`: XNU's `bsd_init` -> `inittodr` calls `IOService::waitForService(IOService::resourceMatching("IORTC"))`. Without real RTC hardware publishing this service, the kernel halts forever waiting for the clock. Publishing it unconditionally unblocks `inittodr`.
- **Risks & Userland Impact:**  
  The system runs strictly on a single core (`CPU 0`). Time of day clock (`gettimeofday`, calendar time) initializes to Unix epoch (1970) or last build timestamp because no real PMIC RTC registers are read.
- **Condition for Removal:**  
  - For SMP: Implement secondary CPU spin-up via Qualcomm ACC / CPU release registers in `OP2PlatformExpert::startCPU()`.
  - For RTC: Write a minimal driver matching Qualcomm PM8994 RTC (`0x02005000` SPMI slave) to publish `IORTC` backed by real hardware registers.

---

## 6. Root Wait Panic Hook (`Milestone 3 SUCCESS -> Milestone 4 Root Mount`)

- **Files:**  
  `iokit/bsddev/IOKitBSDInit.cpp`
- **Why it was necessary:**  
  When booting without a ramdisk or root media, `IOFindBSDRoot()` loops indefinitely: `do { waitForService(); } while (!service);`. On hardware with watchdog disabled, this causes the phone to park at the boot logo indefinitely, requiring battery pulls. In Milestone 3, an explicit panic was inserted right after `Waiting on <dict ...>` to flush logs and trigger `PS_HOLD = 0` warm reset back to TWRP.
  In Milestone 4, this was updated to let `IOFindBSDRoot()` proceed with RAM-disk device matching (`rd=md0`), panicking only if the RAM-disk fails to match (`panic("IOFindBSDRoot: no root device matched (ramdisk missing or failed)")`).
- **Risks & Userland Impact:**  
  The kernel will deliberately panic instead of waiting indefinitely if no root device matches immediately.
- **Condition for Removal:**  
  Once secondary storage drivers (e.g. UFS / eMMC) are ready and asynchronously probed, restore the normal `waitForService` loop.

---

## 7. Qualcomm Watchdog Disabled in Bootloader Shim

- **Files:**  
  `shim/main.c`
- **Why it was necessary:**  
  The APPS SoC hardware watchdog at `0xf9017000` was tested with a deliberate hang. When the watchdog counter expired, rather than triggering a clean warm reset, the phone completely dropped off USB (`adb devices`, `fastboot devices`, and host USB enumeration were all empty) and required a physical battery disconnect/reconnect to recover. The exact underlying cause is unknown—candidates include PMIC power rails latching off, or the device trapping into Qualcomm Emergency Download (EDL / 9008) or RAM-dump mode if download cookies in IMEM (`0xfe80f000` / `0xfe80ffe0`) were armed. The watchdog was disabled in `shim/main.c` to prevent unrecoverable device lockouts.
- **Risks & Userland Impact:**  
  If the kernel hangs in a tight spinlock or fault before panic handling, the phone will stay powered on and warm without automatically rebooting.
- **Condition for Removal:**  
  Investigate IMEM download-mode cookies (`msm_poweroff.c`) and confirm whether clearing them or configuring TrustZone SCM calls allows watchdog expiration to trigger a clean warm reboot. Until proven with an autonomous reboot into TWRP, the watchdog remains disabled in EL1, and all test images must be bounded by deliberate software panic/reset hooks.

---

## 8. Gated PID 1 Syscall Breadcrumbs

- **Files:**  
  `osfmk/arm64/sleh.c`
- **Why it was necessary:**  
  During Milestone 4 bring-up, provides immediate kernel-side breadcrumbs (`PID 1 syscall: %lld`) as PID 1 invokes syscalls, ensuring unmistakable log evidence of userland execution even if the console `/dev/console` write blocks or buffers in the tty layer.
- **Risks & Userland Impact:**  
  Gated behind the boot-argument `trace_syscalls=1`. When the boot-arg is absent (default), it evaluates `false` once and executes zero prints on the syscall fast path.
- **Condition for Removal:**  
  Remove once userland console I/O and process lifecycle are established and reliable.

---

## 9. Built-in Apple HFS+ Driver & macOS 10.14 Compatibility Wrappers

- **Files:**  
  `bsd/op2_hfs/` (core/ and hfs_encodings/)  
  `bsd/op2_hfs/op2_hfs_compat.h`  
  `bsd/op2_hfs/op2_hfs_iokit.c`  
  `bsd/conf/files`  
  `bsd/conf/Makefile.template`  
  `bsd/kern/bsd_init.c`  
  `config/MASTER`  
  `config/MASTER.arm64.msm8994`
- **Why it was necessary:**  
  `xnu-4903` lacks the in-kernel HFS filesystem driver (Apple removed it from the open-source tree in macOS 10.14 and distributed it as a closed-source kext, before open-sourcing the modern implementation in `hfs-627.100.6` under macOS 13). We staged Apple's open-source `hfs-627.100.6` directly into `bsd/op2_hfs/` and linked it built-in. Compatibility shims in `op2_hfs_compat.h` and `op2_hfs_iokit.c` bridge macOS 13 APIs (`IOMallocType`, `IOMallocZero`, `zone_create` -> `zinit`, `struct fsioc_cas_bsdflags`, `vn_getpath_ext` -> `vn_getpath`, `ubc_create_upl` -> `ubc_create_upl_external`, `hashdestroy` -> `FREE`) back to `xnu-4903` (macOS 10.14) KPIs.
- **Risks & Userland Impact:**  
  Content protection (AppleKeyStore) and searchfs are compiled out. Filesystem metadata operates entirely in unencrypted HFS+ mode.
- **Condition for Removal:**  
  Permanent driver for HFS+ root filesystem until APFS support or alternative storage filesystem is brought up.
