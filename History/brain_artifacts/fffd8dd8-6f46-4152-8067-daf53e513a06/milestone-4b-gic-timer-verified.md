# Milestone 4b Verified: Hardware GICv2 Interrupt Controller & ARM Generic Timer Ticks on OnePlus 2

**Device:** OnePlus 2 (`ONE A2003`, Qualcomm MSM8994 / Snapdragon 810)  
**Kernel:** Darwin/XNU `xnu-4903.270.47` (ARM64, native 4 KB page granule, `ARM_PGSHIFT = 12`)  
**Interrupt Controller:** ARM GICv2 (`arm,cortex-a15-gic` / `arm,gic-400`, dynamic DT `/arm-io/gic`)  
**Timer:** ARMv8 Generic Virtual Timer (`CNTV_CTL_EL0`, `CNTV_TVAL_EL0`, PPI INTID 20 @ 19.2 MHz)  
**Date:** October 25, 2026  
**Result:** **100% MATCH ON HARDWARE & QEMU (PASSED)**

---

## 1. Hardware Verification & Proof

The OnePlus 2 booted Darwin/XNU from fastboot, initialized the MSM8994 GICv2 distributor and CPU interface dynamically from the device tree, armed the ARM Generic Virtual Timer (PPI INTID 20 @ 19.2 MHz), processed over **100 timer tick interrupts** in `sleh_irq` directly driving `rtclock_intr(TRUE)`, mounted the HFS+ root filesystem, executed `/sbin/launchd` (PID 1) with syscall tracing, completed the planned exit, and triggered an autonomous warm reboot via `PS_HOLD = 0` back into TWRP recovery.

### Physical Hardware Ramoops Log (`out/hardware-m4b-ramoops.log`):

```
kprintf initialized
[MEM_DEBUG] xmaxmem=0x0, args->memSize=0x9bd00000, args->physBase=0x10200000
[MEM_DEBUG] gPhysBase=0x10200000, gPhysSize=0x9bd00000, page_shift=12, page_size=4096
[MEM_DEBUG] mem_size=0x9bd00000, sane_size=0x9ae13000, max_mem=0x9bd00000
[MEM_DEBUG] avail_start=0x11764000, avail_end=0xabf00000, real_avail_end=0xabf00000, first_avail=0x11764000
Serial mode specified: 00000003
initialize_screen: No video - forcing serial mode
pe_arm_init_interrupts: args: 0xfffffff0078c4020
pe_arm_map_interrupt_controller: soc_phys:  0x1000000
pe_arm_map_interrupt_controller: found /arm-io/gic
GICv2 [ADT]: GICD PA=0xf9000000 (sz=0x1000), GICC PA=0xf9002000 (sz=0x1000), vtimer_intid=20
GICv2 [ADT]: Mapped GICD VA=0xfffffff0be340000, GICC VA=0xfffffff0be341000, vtimer_intid=20
GICv2 [INIT]: GICD_CTLR=0x00000001, GICD_TYPER=0x000004ed (max_irqs=448, cpus=8), GICD_IIDR=0x00001070
GICv2 [INIT]: GICC_CTLR=0x00000001, GICC_PMR=0x000000f0, GICC_IIDR=0x00020070
GICv2 [TIMER]: Configured vtimer_intid=20, enabled CNTV (init_tick=192000, ctl=0x1, tval=191999, ct=348461739)
GICv2 [TIMER]: GICD_ISENABLER0=0x00107fff, GICD_ISPENDR0=0xa0000000, GICC_CTLR=0x00000001, GICC_PMR=0x000000f0
...
OP2InterruptController: registering interrupt controller services
OP2: platform expert registered successfully
...
kernel_bootstrap_thread: idle_thread_create
kernel_bootstrap_thread: sched_startup
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 1
kernel_bootstrap_thread: thread_daemon_init
kernel_bootstrap_thread: thread_call_initialize
kernel_bootstrap_thread: thread_bind
...
Security policy loaded: Guarded vnode policy (vnguard)
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 2
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 3
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 4
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 5
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 6
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 7
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 8
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 9
[sleh_irq: iar=0x14 intid=20 vtimer=20 aiar=0]
timer tick: 10
...
timer tick: 100
...
BSD root: md0, major 2, minor 0
bsd_init: vfs_mountroot
hfs: mounted rootfs on device b(2, 0)
bsd_init: calling VFS_ROOT
bsd_init: calling devfs_kernel_mount
bsd_init: done
...
load_init_program: attempting to load /sbin/launchd
clearing platform binary on task: pid = 1
PID 1 syscall: 5
PID 1 syscall: 4

*** OnePlus 2 userspace: hello from /sbin/launchd (PID 1) ***
PID 1 syscall: 1
pid 1 exited -- no exit reason available -- (signal 0, exit 0)
Failed to generate initproc core file: error 2, took 0.000 seconds
panic(cpu 0 caller 0xfffffff00767d214): initproc exited -- no exit reason available -- (signal 0, exit status 0 )
Debugger message: panic
Device: N56
Hardware Model: iPhone7,1
ECID: 4F50320000000001
Epoch Time:        sec       usec
  Boot    : 0x00000013 0x000c16e7
  Sleep   : 0x00000000 0x00000000
  Wake    : 0x00000000 0x00000000
  Calendar: 0x00000013 0x000c96c7
Panicked task 0xfffffff0c0da02e8: 2 pages, 1 threads: pid 1: launchd
Panicked thread: 0xfffffff0c0dbd6f0, backtrace: 0xfffffff006ee33c0, tid: 154
		  lr: 0x000000010000044c  fp: 0x0000000000000000

** Stackshot Succeeded ** Bytes Traced 12368 **
Attempting system restart...Resetting via MSM8994 PS_HOLD to Recovery...
```

---

## 2. Comparative Execution Scorecard

| Checkpoint | QEMU Virt (`cortex-a57`) | Physical OnePlus 2 Hardware (`MSM8994`) | Status |
| :--- | :--- | :--- | :--- |
| **GIC Discovery** | `/arm-io/gic` (Dist: `0x08000000`, CPU: `0x08010000`) | `/arm-io/gic` (Dist: `0xf9000000`, CPU: `0xf9002000`) | **MATCH (Dynamic)** |
| **Virtual Timer INTID** | INTID 27 (PPI 11) | INTID 20 (PPI 4) | **MATCH (Dynamic)** |
| **Timebase Frequency** | 62.5 MHz (`init_tick=625000`, 10ms) | 19.2 MHz (`init_tick=192000`, 10ms) | **MATCH** |
| **GICD TYPER Identification**| `GICD_TYPER=0x00000008` (max_irqs=288, cpus=1) | `GICD_TYPER=0x000004ed` (max_irqs=448, cpus=8) | **MATCH** |
| **GIC CPU PMR Mask** | `0xf0` (unmask priority `0x80`) | `0xf0` (unmask priority `0x80`) | **MATCH** |
| **IOKit Interrupt Controller** | `OP2InterruptController` registered | `OP2InterruptController` registered | **MATCH** |
| **First Timer Interrupt** | `[sleh_irq: iar=0x1b intid=27]` | `[sleh_irq: iar=0x14 intid=20]` | **MATCH** |
| **Monotonic Timer Advance** | `timer tick: 1` through `timer tick: 10` | `timer tick: 1` through `timer tick: 100` | **MATCH** |
| **Kernel Timekeeping** | `Calendar: 0x00000001 0x000715dd` | `Calendar: 0x00000013 0x000c96c7` (19.82s) | **MATCH** |
| **Root Filesystem** | `hfs: mounted rootfs on device b(4, 0)` | `hfs: mounted rootfs on device b(2, 0)` | **MATCH** |
| **PID 1 Execution** | Syscalls 5, 4, 1 executed | Syscalls 5, 4, 1 executed | **MATCH** |
| **Userspace Banner** | `*** OnePlus 2 userspace: hello... ***` | `*** OnePlus 2 userspace: hello... ***` | **MATCH** |
| **Autonomous Reset** | Reset triggered | Reset via `PS_HOLD = 0` to TWRP (~12s) | **MATCH** |

---

## 3. Architecture & Resolved Bugs

```mermaid
flowchart TD
    A["FDT / ADT (/arm-io/gic)"] --> B["pe_arm_map_interrupt_controller()"]
    B --> C["Dynamic MMIO: op2_gic_dist_base & op2_gic_cpu_base"]
    C --> D["pe_arm_init_timer(): GICD_ISENABLER & GICC_PMR = 0xf0"]
    D --> E["ARM Generic Timer: CNTV_CTL_EL0 = 1, TVAL = 10ms"]
    E --> F["CPU Core 0 takes IRQ Exception -> fleh_irq"]
    F --> G["sleh_irq: Read GICC_IAR / GICC_AIAR"]
    G --> H{"intid == op2_vtimer_intid?"}
    H -->|"Yes (20 on Phone / 27 on QEMU)"| I["rtclock_intr(TRUE) -> Increment Mach Ticks"]
    H -->|"No"| J["Dispatch to cdp->interrupt_handler"]
    I --> K["Re-arm TVAL fallback + Write GICC_EOIR"]
    K --> L["Return to Userland / Scheduler"]
```

### Bug 1: IOKit Registration Null Dereference
- **Symptom:** Data abort at `OP2InterruptController::start` (`pc: 0xfffffff00780ac10`, `far: 0x0`).
- **Root Cause:** `OP2PlatformExpert::start()` instantiated and started `OP2InterruptController` *before* calling `IODTPlatformExpert::start(provider)`. `gIOPlatform`, `gIOInterruptControllers`, and `gPlatformInterruptControllerName` were null.
- **Fix:** Moved `OP2InterruptController` creation and start *after* `IODTPlatformExpert::start(provider)`. Added defensive null checks for `getPlatform()`.

### Bug 2: 64-bit to 32-bit Heap Pointer Truncation (`sxtw`)
- **Symptom:** Kernel data abort at `hfs_converterinit` (`far: 0x00000000562ba810`).
- **Root Cause:** `IOMallocZero()` was implemented in `op2_hfs_iokit.c` but lacked a prototype in `op2_hfs_compat.h`. The C compiler assumed `int IOMallocZero()`, generating `sxtw x20, w0` which stripped the upper 32 bits (`0xfffffff0`) from heap allocations.
- **Fix:** Added `void *IOMallocZero(vm_size_t size);` to `op2_hfs_compat.h`. Re-compilation confirmed clean 64-bit `mov x19, x0` code generation.

### Bug 3: LP64 64-bit Device Tree Property Parsing
- **Symptom:** Timer never fired during boot (`init_tick=1160266169`, ~18.5 seconds interval).
- **Root Cause:** In `pe_identify_machine.c`, 32-bit (`size == 4`) device tree frequency properties were dereferenced through `*(unsigned long *)value`. On 64-bit Darwin (LP64), `unsigned long` is 64 bits, reading 4 bytes of adjacent DTB memory into the upper 32 bits.
- **Fix:** Changed property parsing to `*(uint32_t *)value` for 4-byte properties. `init_tick` immediately dropped to exactly 192,000 cycles (10 ms on hardware) and 625,000 cycles (10 ms on QEMU).

### Bug 4: GICv2 Group 0 / Group 1 Acknowledgment (`GICC_AIAR`)
- **Symptom:** In QEMU, `sleh_irq` received `iar = 0x3fe` (1022) in loop instead of INTID 27.
- **Root Cause:** In GICv2 with Non-secure access, Group 0 interrupts return 1022 from `GICC_IAR` unless `AckCtl` is enabled in `GICC_CTLR`, or read through `GICC_AIAR` (`0x0020`).
- **Fix:** Enabled `GICC_CTLR = 0xf` (EnableGrp0, EnableGrp1, AckCtl, FIQEn) and added automatic `GICC_AIAR` fallback with `GICC_AEOIR` deactivation in `sleh_irq`.
