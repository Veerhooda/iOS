# SMP bring-up plan (MSM8994 4xA53 + 4xA57, XNU xnu-4903)

## Current state

- Boot holds `cpus=1`; ADT has only cpu0 (`tools/mkadt.py`: "Single core
  cpu0 for bring-up").
- `OP2CPU::startCPU` EXISTS and was never tested: SCM set-boot-addr
  (0x02000111, fallback 0x82000111) to `op2_secondary_entry` (PA),
  `unclamp_secondary_cpu` (LDO 0xf9070000+cpu*0x1000, ACC
  0xf908b000+cpu*0x10000 — cluster-0 A53 cores 1..3), then SEV.
- `OP2CPU::startAll` starts APs via `processor_start` with a 100ms
  per-core timeout that SKIPS failed cores and continues. Safe harness.
- `op2_secondary_entry` (osfmk/arm64/start.s:283) jumps to
  `LowResetVectorBase`, which dispatches via MPIDR-matched reset handler
  (`start_cpu`) with x20=boot_args, x21=cpu data. Correct dispatch *if*
  the AP can execute it.

## Gap 1 (BLOCKER): AP MMU bootstrap

TZ releases the AP with MMU OFF at a physical address. `LowResetVectorBase`
immediately loads kernel VAs (`ResetHandlerData`, cpu data) and programs
VBAR with a VA. With MMU off this double-faults and hangs.

Fix: physical mailbox (static phys page, e.g. next to boot_args) filled by
`startCPU` (boot CPU, MMU on) with TTBR0/TTBR1/TCR/MAIR/SCTLR (snapshot of
boot CPU's live values) + `LowResetVectorBase` VA. Rewrite
`op2_secondary_entry` to (all physical): load mailbox, program TTBR/TCR/
MAIR, enable MMU+SCTLR.I+C+M, ISB, branch to the VA. Mirror `__start`'s EL
handling (check EL2 vs EL1 at release; LK/TZ state unknown until tested).

Breadcrumb support: AP writes progress markers (u64) to mailbox slots
before/after each step (MMU on, VBAR set, handler found, start_cpu
reached); boot CPU's startAll logs them after the wait. No console
without MMU, so mailbox is the only early signal.

## Gap 2: per-CPU interrupt + timer init

- GIC-400 CPU interface is banked per core: each AP must enable its own
  GICC (CTLR enable, PMR mask, EOIR/PPIs). `OP2InterruptController` has no
  per-CPU hook today; boot CPU works via one-time init.
- Add AP init (called from `initCPU` on the AP, or `cpu_machine_init`):
  GICC CTLR=1, PMR=0xff, EOIR PPIs; verify SGI (IPI) + virtual timer PPI.
- CNTVOFF_EL2 per AP: verify zero (else virtual timer skewed).

## Gap 3: ADT + boot-args

- Add cpu1 node (reg 1, cpu-id 1, same cluster props as cpu0). Start with
  TWO cores only; cpu2/3 after cpu1 is stable.
- Drop `cpus=1` from the payload cmdline (or set `cpus=2` if XNU honors a
  cap — verify in osfmk/arm64/cpu.c first).
- A57 cluster (cpu4-7): NOT in scope. Different power domains (cluster
  clamp/CCI), HMP scheduling lies to XNU, heat. 4xA53 is the target.

## Gap 4: concurrency audit of OP2 drivers (1-CPU assumptions)

- Framebuffer present path: locks? vsync timer vs present concurrency.
- Exit/spawn rings + snapshot: atomics or locks on all writers.
- I2C poll thread: single owner OK, but probe-vs-thread ordering.
- Interrupt controller: per-CPU state, no shared mutable without lock.
- panic/deadline path with APs wedged.

## Test plan (each step a short boot)

1. cpu1 node + `cpus=2`(or drop cap): expect startCPU logs (SCM rc),
   mailbox breadcrumbs, "came online" or clean 100ms skip. No hang.
2. AP online: verify timer interrupts + IPIs on cpu1 (kprintf from AP
   contexts), scheduler migration (threads run on cpu1).
3. cpu2/3, then soak: 30-min userland run, compare crash census vs 1-CPU.
4. Perf: measure boot-to-home time and UI responsiveness vs 1 CPU.

## Risks

- SCM call wrong (args/convention) can wedge TZ or the target core; the
  startAll timeout only covers "AP never runs", not "AP runs amok".
  Mitigate with mailbox breadcrumbs + minimal first release.
- TZ may release APs at EL2 (need EL drop) or with caches in a state that
  needs maintenance; inspect `__start` and mirror exactly.
- A runaway AP can corrupt memory (wrong TTBR) — triple-check mailbox
  values against live boot-CPU system registers before first release.
- Big win, big risk: this is the highest-leverage speed work and the
  highest-risk kernel work. Keep every step boot-verified and reversible.
