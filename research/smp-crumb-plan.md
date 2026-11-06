# SMP repro crumb plan (post-stability; run134 hung with no USB/ramoops)

## Why run134 told us nothing
Hang = no ramoops (panic path never ran), no USB (likely stuck before/without
WD). Need IN-RAM crumbs readable... but we can't read RAM without USB/panic.
So: crumbs must ESCAPE via (a) forced panic after timeout, or (b) pstore
write + WDT reset (pstore survives reboot, readable in TWRP via adb).

## Patch list (all in OP2CPU.cpp / start.s AP path)
1. `op2_ap_crumb[N]` pstore-persistent u64 array (reuse ramoops carveout tail,
   OUTSIDE console region): AP index writes 1=entered, 2=MMU on, 3=PSCI done,
   4=first IPI, 5=idle loop. Each write followed by DSB + cache clean to PoC
   (else invisible post-reset).
2. CPU0: after AP boot kick, 5s watchdog thread: if crumb[1]==0 -> OP2EMMC
   STATS + crumb dump via IOLog + `PE_halt_restart(0)`?? NO - need PANIC for
   ramoops: call `panic("op2 ap1 no-boot crumb=%llx")` (panic reboots to TWRP
   via the existing panic->reset path; ramoops holds the dump).
3. PSCI CPU_ON return code logged (denied/invalid -> SCM firmware rejects).
4. IPI test BEFORE declaring SMP up: cpu0 sends test IPI, waits 1s for ACK
   crumb; on timeout panic with crumbs (distinguishes boot-vs-IPI failure).
5. Keep cpu0-only ADT until (2)-(4) pass; then cpu1, soak 10 min, then cpu2/3.

## Hypotheses ranked (MSM8994 specifics)
- H1: APs need SCM/secure kick (PSCI CPU_ON via SMC, not SEV/mailbox). Our
  start.s may use the wrong entry (check aboot's AP release addr = the
  `cpu-release-addr` aboot leaves in its DTB).
- H2: AP MMU/CPUACTLR setup missing MSM8994 errata bits (check 8994 TRM
  CPU power/cluster bits vs our start.s).
- H3: GICR/GICD redistributor for APs not woken (GICv2 on 8994? then CPU
  interface per-core enable missing).
- H4: TLB/cache coherency: AP pagetable walk with stale caches (do full
  PoC clean + TLBI ALL + DSB/ISB on cpu0 BEFORE kick, DSB on AP entry).

## Go criterion
AP1 crumb walks 1->5 + 10-min 2-core soak with 0 hangs + touch still framed.
Only then cpu2/3. See also research/digest-smp-plan.md.
