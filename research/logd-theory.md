# logd crash theory (run141/run142) - PARKED, boot-storm-only

## Facts
- Site (both runs, same): libsystem_platform + 0x7da4, inside memmove's LDP loop.
- Args: dst = own stack buffer, src = 0x16f/0x170-page-aligned UNMAPPED page,
  len ~0x360. SIGSEGV translation fault (EL0).
- Caller chain: logd -> malloc -> libicucore (string format) -> [tail-call] memmove.
- Timing: pids 167-173 (BOOT storm). Post-boot: ~2 deaths / 2h (rare).
- Neighbors: SpringBoard Extension-loading logs surround 2/3 visible crashes.

## Theory
logd formats a message containing a DANGLING pointer into a dead thread's
(unmapped) stack page - ICU memmove faults. Boot log storm + 1-core timing
skew = extreme thread churn -> 14 crashes at boot, then quiet.

## Why parked
- Post-boot impact ~nil (2/2h). UI daemons unaffected.
- Fix needs logd/ICU binary surgery (same trampoline class as hunkQC) or
  sender-side (SpringBoard log content) changes - both high-cost, low-payoff.
- Revisit if: post-boot logd rate rises, or logs become load-bearing.

## If revisited
- Confirm dangling-stack via logd thread-churn correlation (spawn/exit of
  logd worker threads around crashes - needs finer logging).
- Mitigation (cheap): quieter boot args (fewer messages -> less churn).
