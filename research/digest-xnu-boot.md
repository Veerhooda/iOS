# XNU early-boot hang digest: stuck at Apple logo, no verbose output

Scope: iOS 12-era XNU (`xnu-4903`, arm64) boot contract and external reports on
hangs before/without console output, in jailbreak / QEMU / porting / hackintosh contexts.

## 1. `boot_args` contract (arm64, xnu-4903.221.2)

Source: `pexpert/pexpert/arm64/boot.h` at tag `xnu-4903.221.2`
(<https://raw.githubusercontent.com/apple/darwin-xnu/xnu-4903.221.2/pexpert/pexpert/arm64/boot.h>):

- `Revision` / `Version` (`uint16_t` each): `kBootArgsRevision 1`,
  `kBootArgsRevision2 2` (added `bootFlags`); `kBootArgsVersion1/2` = 1/2.
- `virtBase`, `physBase`, `memSize`, `topOfKernelData` (`uint64_t`):
  virtual/physical memory base, memory size, highest physical address used by
  the kernel data area (kernel + ramdisk + device tree + boot args must be
  below it).
- `Video` (`struct Boot_Video`): `v_baseAddr`, `v_display`, `v_rowBytes`,
  `v_width`, `v_height`, `v_depth` (depth/rotate/scale packed; depth mask
  `0xFF`, rotate shift 8, scale shift 16; no `BootRotate` shift in 4903).
- `machineType` (`uint32_t`).
- `deviceTreeP` (`void *`, **physical** address) + `deviceTreeLength`
  (`uint32_t`): base/length of the flattened (Apple-format) device tree.
- `CommandLine[BOOT_LINE_LENGTH]`, `BOOT_LINE_LENGTH = 256` in 4903
  (608 on current master).
- `bootFlags` (`uint64_t`): `kBootFlagsDarkBoot = (1ULL << 0)`.
- `memSizeActual` (`uint64_t`).

## 2. `PE_parse_boot_argn` / `PE_init_platform` / iBoot expectations

- Boot-arg parsing is string matching over `boot_args.CommandLine`
  (`PE_parse_boot_argn(name, ptr, len)`); kexts/iokit match drivers on DT
  nodes iBoot populated. API surface:
  <https://github.com/stek29/stek29.github.io/blob/HEAD/_posts/2018-01-22-lwvm-mapforio.md>
  (usage) and Apple docs
  <https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/services/services.html>.
- `-v` is **not** an XNU boot arg: it is intercepted by iBoot. Boot-arg
  injectors that run after iBoot (PongoOS) cannot enable verbose that way;
  jailbreak tools need an explicit verbose-boot path (palera1n `-V`):
  <https://cdn.nickchan.lol/palera1n/c-rewrite/releases/v2.0.0-beta.2/palera1n.1.html>.
- `deviceTreeP` must be a **physical** address: `PE_init_platform()` stashes
  it raw and `PE_slide_devicetree()` adds `(gVirtBase - gPhysBase)`:
  <https://github.com/anurodhp/qemu-xnu-arm/commit/ae5edc0aa8b42ba60337bbc046292288e6b1482a>.
- `physBase` must be **nonzero**: `pmap_bootstrap()` sets
  `vm_first_phys = gPhysBase` and `patch_low_glo_static_region()` asserts
  `vm_first_phys != 0` (0 = sentinel). Same commit as above.
- Known-good QEMU bring-up cmdline: `debug=0x8 kextlog=0xfff cpus=1 rd=md0`:
  <https://worthdoingbadly.com/xnuqemu2/>. "All XNU needs to boot into
  userspace is a serial port and a timer" (same source).
- palera1n reserves `rootdev=md0` and uses `wdt=-1` during fakefs setup;
  custom XNU args via `-e`:
  <https://cdn.nickchan.lol/palera1n/c-rewrite/releases/v2.0.0-beta.2/palera1n.1.html>.

## 3. DTB-vs-Apple-Device-Tree conversion

- iBoot/XNU use Apple's own flattened format, **not FDT**: node = counts +
  properties + children; property = name[32] + length + 4-byte-padded value;
  mask off the top bit of length (tooling flag). The kernel does not boot
  without it:
  <https://github.com/j0shua-syson/ios3-vm/commit/fa83c4a9fbbdfe1c6550f54c5f7ab76e873ef006>.
- Porting shims translate stock FDT into ADT (`mkadt.py`: ADT generated from
  stock DTB):
  <https://github.com/janeawa/ios16_on_mix2s>;
  DTB-to-ADT translation + `boot_args` construction in a bootloader shim:
  <https://github.com/lechaukha12/xnu-xzs>;
  historical equivalent (flatten plist device tree, append memory nodes, attach
  DT + kernel to shim like a `dtbImage`):
  <http://winocm.com/xnu/projects/2013/07/16/porting-xnu-to-arm/>.
- QEMU loaders must patch ramdisk + trust-cache addresses into the DT at load:
  <https://alephsecurity.com/2019/06/25/xnu-qemu-arm64-2/>.

## 4. kernelcache vs raw kernel

- Normal iOS boot: iBoot loads a **kernelcache** (pre-linked kernel + kexts,
  compressed container) over USB/TFTP in development flows:
  <http://winocm.com/xnu/projects/2013/07/16/porting-xnu-to-arm/>.
- On-device format is IMG4/IM4P (`krnl` payload), LZFSE/LZSS compressed,
  modern caches use MH_FILESET; raw Mach-O recovery via
  `img4 -i kernelcache.release.* -o kcache.raw` (+ `-T rkrn` for restore
  kernels):
  <https://github.com/machxnu/machxnu.github.io/blob/HEAD/_posts/2022-05-07-verbose-restore-64-bits.md>;
  background:
  <https://github.com/hinana0325/siliconv/blob/HEAD/spec/boot/apple-boot-flow.md>.
- KPP (non-KTRR) devices ship a **secure monitor** that must be loaded and
  entered in EL3 with its own boot args; it parses the kernel Mach-O header at
  the kernel base taken from the kernel boot args (`virt_base` must point at
  the Mach-O header). Skipping EL3 works only until the first SMC:
  <https://alephsecurity.com/2019/06/25/xnu-qemu-arm64-2/>.

## 5. Early-boot diagnostics (getting output when the screen is dead)

- Serial: `serial=1` output, `serial=2` input, `serial=3` both
  (`SERIALMODE_OUTPUT/INPUT/SYNCDRAIN/BASE_TTY/NO_IOLOG` bits 0-4):
  <https://dreamwhite.dev/OpenCorePkg-Documentation/docs/NVRAM/other_variables.html>.
  On iOS/QEMU, `serial=2` (bit #1) enabled interactive UART shell:
  <https://alephsecurity.com/2019/06/25/xnu-qemu-arm64-2/>.
- Debug mask: Dortania kernel-debugging recipe
  `-v keepsyms=1 debug=0x12a msgbuf=1048576` =
  `DB_PRT(0x2)|DB_KPRT(0x8)|DB_SLOG(0x20)|DB_LOG_PI_SCRN(0x100)`; full bit list
  in `osfmk/kern/debug.h`:
  <https://github.com/dortania/opencore-install-guide/blob/HEAD/troubleshooting/kernel-debugging.md>.
  `0x08` streams kprintf to serial; `0x100` disables the graphical panic
  dialog (keeps panic text on screen):
  <https://newbe.dev/index.php/is-there-a-list-of-available-boot-args-for-darwin-os-x>.
- Jailbreak: verbose via tool flag (`palera1n -V`), custom args via `-e`
  (`rootdev=md0` reserved, `wdt=-1` disables watchdog during setup):
  <https://cdn.nickchan.lol/palera1n/c-rewrite/releases/v2.0.0-beta.2/palera1n.1.html>.
- ramoops equivalent: iOS persists panics as `panic-full-*` under
  Settings > Privacy > Analytics > Analytics Data:
  <https://www.ifixit.com/Wiki/iPhone_Kernel_Panics?mibextid=xfxF2i>.
- Hypervisor/JTAG fallback: ARM DS-5 + FVP used to debug iOS kernel on
  non-Apple hardware:
  <https://forum.khadas.com/t/run-the-unmodified-ios-kernel-image-on-the-khadas-vim3-1/7621/1>.

## 6. Cause table: stuck at Apple logo, no verbose output

| # | Cause | Source | How to diagnose | Fix |
|---|-------|--------|-----------------|-----|
| 1 | `-v` passed as XNU arg does nothing (consumed by iBoot) | <https://cdn.nickchan.lol/palera1n/c-rewrite/releases/v2.0.0-beta.2/palera1n.1.html> | Logo stays, no text despite `-v` in cmdline | Use the loader's verbose-boot path (`palera1n -V`); route console to serial instead |
| 2 | No console routed anywhere (framebuffer dead, serial off) | <https://worthdoingbadly.com/xnuqemu2/> | Attach serial: if logs flow, kernel is alive | `serial=3`, `debug=0x8` (`DB_KPRT`), `keepsyms=1`, `msgbuf=1048576` |
| 3 | `deviceTreeP` passed as virtual address | <https://github.com/anurodhp/qemu-xnu-arm/commit/ae5edc0aa8b42ba60337bbc046292288e6b1482a> | Early panic/hang in `PE_init_platform`/`PE_slide_devicetree` path; DT unreadable after slide | Pass physical address of the flattened tree |
| 4 | `physBase == 0` (real DRAM base is 0) | <https://github.com/anurodhp/qemu-xnu-arm/commit/ae5edc0aa8b42ba60337bbc046292288e6b1482a> | Assert in `patch_low_glo_static_region` (`vm_first_phys != 0`) | Load kernel/DT/boot_args at a nonzero physical base; set `physBase` accordingly |
| 5 | FDT passed instead of Apple flattened DT, or malformed ADT | <https://github.com/j0shua-syson/ios3-vm/commit/fa83c4a9fbbdfe1c6550f54c5f7ab76e873ef006> | Hang in `PE_init_platform`/`DTInit` with no driver matches | Convert DTB to Apple format (node counts+props+children, name[32], padded values, mask length top bit); validate whole tree up front |
| 6 | Missing DT nodes (uart/timer/memory) | <https://worthdoingbadly.com/xnuqemu2/> | No serial output at all; hang before kprintf | Minimal DT needs memory + serial + timer; add nodes, fix `reg`/clock props |
| 7 | `topOfKernelData` wrong (ramdisk/DT/boot-args outside mapped range) | <https://alephsecurity.com/2019/06/25/xnu-qemu-arm64-2/> | `arm_vm_page_granular_RWNX` region wrong; DT/ramdisk/trust-cache unreadable | Set to highest physical address used by kernel data area incl. DT, ramdisk, boot args |
| 8 | Secure monitor missing on KPP device (EL3) | <https://alephsecurity.com/2019/06/25/xnu-qemu-arm64-2/> | Boots from EL1 entry then hangs/crashes at first SMC | Load secure-monitor image, start in EL3 at its entry with its own boot args; `virt_base` must point at kernel Mach-O header |
| 9 | Encrypted/compressed kernelcache booted as raw kernel | <https://github.com/machxnu/machxnu.github.io/blob/HEAD/_posts/2022-05-07-verbose-restore-64-bits.md> | Garbage entry / immediate exception; not a Mach-O at entry | `img4 -i kernelcache... -o kcache.raw` (decrypt with firmware keys first if needed); boot resulting Mach-O/MH_FILESET member |
| 10 | Wrong root device (ramdisk not selected) | <https://worthdoingbadly.com/xnuqemu2/> | Boot proceeds then stalls waiting for root | `rd=md0` (+ `rootdev=md0` in jailbreak flows); ramdisk address patched into DT |
| 11 | Watchdog reboots before panic is visible | <https://cdn.nickchan.lol/palera1n/c-rewrite/releases/v2.0.0-beta.2/palera1n.1.html> | Boot loop with no text; panic never readable | `wdt=-1`; `debug=0x100` to keep panic on screen; capture serial log |
| 12 | `secure-root-prefix` DT prop triggers unpatched platform-function path | <https://alephsecurity.com/2019/06/25/xnu-qemu-arm64-2/> | Hang/return-early in `callPlatformFunction` after DT edits | Omit the prop (keeps offset-0x110 member 0, path returns cleanly) instead of patching the kernel |
| 13 | Static trust cache missing/misplaced (own binaries won't exec) | <https://alephsecurity.com/2019/06/25/xnu-qemu-arm64-2/> | Kernel boots but launchd/services fail to exec; trust-cache parse errors | Load trust cache in memory, point to it from DT; place inside mapped kernel-data region (`segEXTRADATA` logic) |
| 14 | UART input disabled (looks hung, is actually waiting) | <https://alephsecurity.com/2019/06/25/xnu-qemu-arm64-2/> | Output flows but no interaction possible | `serial=2` (bit #1 = `SERIALMODE_INPUT`) |
| 15 | Framebuffer/video args wrong (logo, no progress) | <https://raw.githubusercontent.com/apple/darwin-xnu/xnu-4903.221.2/pexpert/pexpert/arm64/boot.h> | Logo shown, never advances, serial shows continued boot | Fix `Boot_Video` (`v_baseAddr`/`v_rowBytes`/dims/depth); confirm with serial that the kernel is past video init |
| 16 | (Hackintosh analog) console/memory-map handoff wrong | <https://dortania.github.io/OpenCore-Install-Guide/troubleshooting/extended/kernel-issues.html> | Stuck at `EndRandomSeed` / `[EB|#LOG:EXITBS:START]` with no kernel output | `ProvideConsoleGop`; `SetupVirtualMap` / `RebuildAppleMemoryMap` + `SyncRuntimePermissions` combo; whitelist `DevirtualiseMmio` regions |
| 17 | (Hackintosh analog) BIOS update changed memory map | <https://www.tonymacx86.com/threads/solved-stuck-on-apple-logo-without-progress-bar.328052/> | Worked before, now logo with no progress bar after firmware change | Re-tune Booter quirks (`SetupVirtualMap=true` in the reported case); re-check `slide=` |
| 18 | Panic occurred but evidence lost (no persistence) | <https://www.ifixit.com/Wiki/iPhone_Kernel_Panics?mibextid=xfxF2i> | Device reboots, nothing on screen | Read `panic-full-*` in Analytics Data (iOS ramoops equivalent); keep serial log as primary |

## Gaps / unverified

- `theiphonewiki.com/wiki/DeviceTree` (iBoot DT population + IMG3/IM4P
  packaging notes) failed to fetch twice (HTTP 504); ADT format claims above
  rest on the fetched ios3-vm parser source instead.
- `PE_init_platform` body for xnu-4903 was not fetched line-by-line (relied on
  the qemu-xnu-arm commit's traced description of `PE_slide_devicetree` and
  `pmap_bootstrap` behavior).
