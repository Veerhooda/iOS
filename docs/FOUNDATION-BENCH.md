# The foundation, measured (2026-11-12, rounds fable-41/42)

The UI was slow on the CPU builds too ("Home dead for 2-3 minutes after boot, Settings lags"), and backboardd's
shader translation was already off the critical path (the warm cache, FABLE-REVIEW K). So: measure the base.
`userland/op2bench` runs as tzinit (tools/deploy-probe.sh), forks away from launchd, and logs `BENCH` lines 150 s and
~5 min after boot. Both phases agree within a few percent.

| | measured | read |
|---|---|---|
| CPU, dependent adds | 0.64 /ns; 1.71 /ns with 4 chains | right for an in-order A53 at 1.5 GHz |
| 4 CPUs | no slowdown with 3 spinners | all four run |
| latency L1 / L2 / DRAM | 2.3 / 13 / ~200 ns | DRAM a little slow (no DDR/bus vote is made), not drastic |
| thread wake, condvar round trip | ~51 us | fine (IPIs work) |
| process wake, pipe round trip | ~21 us | fine |
| usleep 1 ms / 16 ms | 1.14 / 17.0 ms | timers fine |
| getppid | ~720 ns | slowish, not a stall |
| **storage, sequential 1 MB** | **16.7 MB/s** | eMMC 5.0 HS200 does ~150-250 MB/s with DMA |
| **storage, random 4 KB** | **~360 us** | |
| **storage, 16 KB (a page)** | **~640 us, 25 MB/s** | |

**Root cause of the lag: storage.** `bsd/dev/op2_emmc.c` reads by polled PIO (CMD17/CMD18, the CPU pulling
SDHCI_BUFFER one 32-bit word at a time and spinning between). Every page fault, every app launch (thousands of pages
of the shared cache), every daemon relaunch (dataaccessd crashes ~every 27 s, logd and five Spotlight importers loop
too) waits at ~640 us a page and holds a CPU while it waits. Wallet was killed by the launch watchdog (0x8badf00d)
blocked in mach_msg, i.e. waiting on a daemon that was itself paging in.

Next, in order: SDHCI DMA for reads (a physically contiguous bounce buffer and SDMA/ADMA2, a boot self-test against
PIO like the CMD18 one, writes stay PIO at first); then stop the crash loops (dataaccessd, logd, Spotlight
importers); then the A57 cluster (Cortex-A53 only today) and a DDR/bus bandwidth vote.
The memcpy figure the probe prints is broken (the compiler drops the copies); ignore it.

## ADMA2 reads (round fable-43)

`op2_emmc.c` `sdhc_read_dma`: CMD18 up to 256 sectors into a 128 KB contiguous bounce buffer through a 32-bit ADMA2
descriptor table, DATA_END polled, buffer invalidated, memcpy out. Boot self-test (32 and 256 sectors, DMA against
PIO, byte for byte) gates it; `op2_emmc_dma=0` turns it off; repeated failures turn it off. 15695 DMA commands, 0
failures in the first boot.

| | PIO | ADMA2 |
|---|---|---|
| sequential 1 MB | 16.7 MB/s | **42 MB/s** |
| 16 KB page | ~640 us | **~285 us** |
| random 4 KB | ~360 us | ~335 us (card + command latency) |

42 MB/s on an 8-bit bus is what high-speed SDR at 52 MHz gives: the bootloader's bus mode (the driver keeps the
card's TRAN state). HS200 (200 MHz, tuning, 1.8 V I/O) or DDR52 would be the next 2-4x; the boot log now prints
the clock/host-control registers to confirm the mode first.
