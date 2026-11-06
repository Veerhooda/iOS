# SDHCI per-boot bad state (run131, 2026-11-04)

## Observation
Identical kernel + image across boots: run128/129/130/132 show ZERO eMMC
failures over ~2M sector reads each, but run131 showed 2117 read + 344
write failures (ALL rc=-2 = CMD response timeout/error, never data-phase
-3/-4), retry_ok=1 (retries never recover: sticky bad state, not transient
media errors). Same LBAs fail repeatedly; many distinct LBAs affected.

## Impact
Bad boots kill the logging daemons (run131: logd 90x SEGV NULL+0x40 at a new
site, syslogd 9x SIGBUS) but the visible system survives (SpringBoard 0,
locationd 0, touch works). Each -2 burns ~100ms spinning, so bad boots also
lose minutes of CPU to timeouts (2461 fails ~= 4 min in run131).

## Hypothesis (unproven)
Per-boot SDCC PHY/DLL/clock leftover state: the kernel driver never programs
the MSM DLL, pads, or PWRCTL, trusting aboot/LK/TWRP state. The shim already
notes HS200 leftovers ("Resetting eMMC 5.0 card out of HS200 into Idle").
Some boots lock marginally -> response CRC/timeout errors under load.

## Mitigation applied
- SDHCI DATA_TIMEOUT max (0xE): write-once at init (timeout was 328us).
- Read retry limited to data-phase (-3/-4); response errors (-1/-2) fail
  immediately (retry never recovers them, each attempt wastes ~100ms).

## Still open
- True fix: program DLL/pads/PWRCTL in sdhc_init_card (see Linux sdhci-msm
  tuning), or full controller re-init after N consecutive failures.
- Frequency estimate: 1 bad boot in ~5 (run128-132). Reboot as workaround.
