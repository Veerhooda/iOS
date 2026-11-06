# Sleep-gate win + bad-boot contamination (loop-1/loop-2, Nov 6)

## Sleep is dead (hakios gate works)
- Loop-1 (aggressiveness vetoes only): phone froze mid-run, deadline never
  fired, no USB. Timer frozen = FULL SYSTEM SLEEP (powerd asks within
  seconds on lock screen; aggressiveness veto bypassed).
- Loop-2 (gSleepDisabledFlag ported from hakios/8796 to 4903): reset ON
  SCHEDULE (03:21), AUTOTAP 34/34 (awake the whole run).
- Port: flag default 1 (MSM8994-gated) + syssleep boot-arg + first-check in
  checkSystemSleepAllowed (err 7). Verified: 'syssleep' in binary.

## Bad-boot contamination (loop-2)
- eMMC: 6591 rd_fail + 184 wr_fail (rc=-2 class, retry_ok=0) = the known
  per-boot SDHCI bad PHY state (cf run131). Fresh-bad luck, unrelated to
  the gate (no eMMC path touched).
- Effect: xpcproxy 148 + logd 108 deaths (binaries/logs unreadable).
  backboardd pid45 0 deaths, SB 0 (UI stack immune even when starved).
- Rule: ANY run with fails_recorded>0 is contaminated for stability
  verdicts; reboot for a clean roll (loop-3). Two bad-boots in a row =
  investigate (not luck).
