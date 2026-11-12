# GPU plan (2026-11-11)

Goal: CoreAnimation's render server (backboardd) draws through Metal on the Adreno 430,
so UI compositing, blur and animation leave the CPU.

## What exists
- Kernel `OP2GPU` (iokit/Kernel/arm/OP2GPU.cpp): RPM rail vote, GDSC, CP microcode, ringbuffer,
  `OP2GPUUserClient` with an msm_drm-like ABI. Verified only: CP_NOP and an indirect buffer +
  fence. No draw, no shader has ever run.
- Userland `OP2Metal.bundle`: a Metal *shell*. Device/queue/texture objects exist, nothing is
  drawn. With metal=2, CA routes to it and frames come out blank, hence metal=0 today.

## The reference that makes this feasible: hakios (MIT, github.com/oosexgang/hakios-xnu-kernel)
iOS 16 on an Adreno 6xx phone. It has, measured on hardware:
- AIR -> NIR front end (tools/mesa/air) + Mesa ir3: Apple's own .metallib shaders compiled on
  device. All 39 VS + 95 FS of iOS 16 CoreAnimation compile.
- Metal plug-in + C++ core: buffers, textures, samplers, compute, render passes, clear, blend,
  MRT, depth, through msm DRM requests to its kernel driver.
- Not yet: CoreAnimation actually drawing through it.

## What changes for us
| Layer | hakios (A6xx) | OnePlus 2 (A4xx) |
|---|---|---|
| Compiler | AIR->NIR->ir3, chip 0x06030500 | same code, ir3 for chip 0x04030002 (ir3 supports a4xx; needs checking per op) |
| Packets | PM4 type 4/7 | PM4 type 3 (fd4) |
| Draw state | turnip/fd6 registers | fd4 (src/gallium/drivers/freedreno/a4xx/fd4_emit.c, fd4_draw.c, fd4_gmem.c) |
| Kernel ABI | HakiosGPUUserClient msm selectors | OP2GPUUserClient: align to hakios selectors |
| iOS | 16 (AIR newer) | 12.5.7 (older AIR bitcode: front end may need work) |

## Stages (each ends with a test on the phone)
- G0  Kernel: GPU clocks at a real rate, IOMMU/SMMU state known, IRQ + fence, GPU hang recovery.
      Test: 1000 submissions with fences, no hang.
- G1  CP blits: CP_MEMCPY / CP_MEM_WRITE fill + copy through the hakios C++ core, type-3 packets.
      Test: hakios-metal-probe copy/fill lines, byte exact.
- G2  Compiler: Mesa 25.0.x + AIR front end, host build on the Mac; sweep iOS 12 QuartzCore
      default.metallib for chip 0x04030002. Test: count of CA functions that compile.
- G3  First triangle: fd4 sysmem draw into an RGBA8 target. Test: probe stage 9 pixel rule.
- G4  Textures, blend, clear, MRT: the subset CA's pipelines use.
- G5  CA on GPU: OP2Metal replaced by the ported plug-in, metal=1, render server targets an
      IOSurface; display scales it with the MDP5 VIG scaler (no CPU copy).

## G0 results (2026-11-11): PASSED on the phone (logs/daily-20261111-044636.log)
    OP2 GPU: write probe idle=1 MEM_WRITE#1=OK MEM_WRITE#2=OK CP->reg=OK REG_TO_MEM=OK MEM_WRITE->ring=OK
    OP2 GPU G0: serial 1000/1000 ok, 0 wrong-data, avg 35 us max 40 us; burst 1000 ok in 1603 us
    OP2 GPU G0: hang test: hung=1 recovered=1 post-recovery submit=ok (scratch=0xbeef0001)
Kernel: iokit/Kernel/arm/OP2GPU.cpp (old file: kernel-backup/round6-gpu/, current: kernel-backup/round6-gpu/after-g1/).
Boot-args: gpu_khz (300000), gpu_smmusec (1), gpu_init (bitmask, 1 = VBIF), gpu_selftest (1000),
gpu_hangtest (1), gpu_smmu (1 = read context banks), gpu_evtest (0), gpu_mhz (1).

What was wrong before (each measured, ~20 boots):
1. **The micro-engine never ran.** CP_ME_CNTL was "halted" with 1; the halt bits are 27|28 (KGSL).
   With ME RAM loaded while not halted, the PFP still fetched (RPTR moved, "ME_INIT accepted"),
   but no packet executed: not even a type-0 register write. Packets piled up in the ME queue,
   PFP stalled after ~25. The old "CP_NOP / IB + fence verified" milestones were RPTR artefacts.
   Microcode readback (ME 2388 + PFP 1072 words) is exact; it matches /system/etc/firmware.
2. **The GPU SMMU is TZ-owned** (secure id 18). Needed, in this order, once per boot (all ACK):
   TZ MP feature version (1.1.1) -> SET_CP_POOL_SIZE(300 MB) -> PTBL_SIZE (148 KB) ->
   PTBL_INIT(pool) -> restore_sec_cfg(18, ctx 0) and (18, ctx 1). SMC: 32-bit SIP fn 0x02000Cxx.
   Afterwards the context banks (0xfdb18000 +0x1000/CB) are readable: SCTLR=0 (bypass, physical
   addresses), no faults. The SMMU *global* space stays secure: reading it resets the SoC.
   Before the SMC, writing VBIF resets the SoC; after it, KGSL's a430_vbif[] goes in fine.
3. **GPU core clock is RPM-owned** (clk2 id 1, KHz). Without the vote, core-clocked blocks hang the
   bus on write. 300 MHz vote ACKs; RPM sets VDD_GFX itself.
4. **CP_EVENT_WRITE(CACHE_FLUSH_TS) never retires** with the 3D pipe uninitialised, and after one
   such event no later memory write lands (register writes still do; soft reset does not clear it).
   Fences are therefore CP_MEM_WRITE (fence mode "memwrite"). Revisit in G3 once the pipe is set up
   (KGSL's fence is CACHE_FLUSH_TS; gpu_evtest=1 runs the test).
5. Hang detection must watch retired fences, not RPTR: the PFP prefetches past a stuck IB.

Still open in G0:
- GPU IRQ (SPI 33 = GIC 65) not wired; waits poll (spin 2 ms, then IOSleep(1)).
- Core clock not measured: RBBM perf counter 0 reads 0 cycles (select/enable incomplete).
- UCHE (gpu_init 4), SP regfile sleep (8), hang-detect (2), SP/TP power (16) were only ever tried
  before the SMC/ME fixes, when they led to resets; retest one at a time. SP/TP is needed for G3.
- Header bug fixed: UCHE_CACHE_WAYS_VFD is 0xe8c, UCHE_TRAP_BASE 0xe83/0xe84 (was 0x384/0x386/0x387).
- The latched AHB error 0x63c decodes to a READ of word 0x18f (harmless).

## G1 results (2026-11-11): PASSED on the phone, from userland (logs/daily-20261111-051759.log)
Userland: userland/op2gpu/ (gpu layer adapted from hakios-metal, MIT; A4xx type-3 packets; blits; fake GPU; probe).
    make -C userland/op2gpu test     # host: the same checks against the fake GPU, + mutations it must refuse
    make -C userland/op2gpu ios      # iOS 12 probe (Xcode 10.3 toolchain on PATH), signed com.apple.tzinit
    tools/deploy-probe.sh            # phone in TWRP: probe -> /usr/libexec/tzinit (in place); --restore puts it back
On the phone (probe as tzinit, lines via the new kernel selector kOP2Log 0x7c -> ramoops):
    fill: 60 of 60 (length, offset) pairs byte exact
    copy of 500000 bytes: byte exact, 29103 us, hash 110f5905 (= the host's)
    copy: unaligned pairs, one word, odd word count: 3 of 3; 40 batches in order: 0 words wrong
    fill of 1 MB: exact in 1774 us (591 MB/s); no GPU reset during the checks
Measured A4xx CP primitives (kernel self-test G1, logs/daily-20261111-050355.log):
- CP_MEM_WRITE inline: 16000 words in 104 us. Fills are inline writes (command stream = fill size).
- CP_MEM_TO_MEM (0x73) exists on A4xx, 32-bit and 64-bit (flag bit 29): 4096 words in 907 us, ~0.44 us/packet
  (~17 MB/s). Correct but slow: bulk copies belong to the 2D/3D engine (G3/G4).
- CP_MEM_TO_REG/REG_TO_MEM with a count of 8 hung the CP (AHB write to word 0x590): count encoding unknown, unused.
- No CP_MEMCPY before A6xx.
Kernel ABI now as hakios/msm expect: fence/cpu_prep timeouts are absolute deadlines (mach_continuous_time ns),
SubmitQueueNew/Close accepted (one ring), GetParam FAULTS(9)=GPU resets, buffers mapped write-combined to
userland, every BO in a submission carries its fence. The original tzinit is in backup/rootfs/tzinit.orig
(restored on the phone after the run).

## G3 results (2026-11-11): first draw on the A430, pixel exact (logs/g3-air-run-3.txt)
Mesa's own fd4 driver, cross-built for iOS 12, on the phone through our kernel:
    air_run: clear_frag on FD430 (the GPU, through the kernel's msm device)
    air_run: the pixel at (0, 0) of render target 0 came back as:  0.000988960266 0.0119247437 0.130004883 1.55371094
clear_frag returns a half4 loaded from its constant buffer, which air_run fills with bytes (i*7+13)&0xff =
0d141b222930373e: as half4 that is exactly 0.0009889603, 0.0119247437, 0.1300048828, 1.5537109375. One submit
(1 command buffer, 16 buffers), no hang, no reset. (softpipe's own answer, 0 0.1788 0 0, is the wrong one: its
half-float uniform path.) iOS 12 QuartzCore shader -> AIR front end -> ir3 -> fd4 sysmem draw (FD_MESA_DEBUG=sysmem).
How it is built and run:
    bash tools/mesa-ios.sh            # Mesa 25.0.7 for iOS 12 arm64 (this Mac's clang, Xcode 10.3's iPhoneOS 12.4 SDK +
                                      # libc++ headers, tools/ios-iokit headers): freedreno + hakios-drm -> OP2GPU, air_run
    bash tools/rootfs-journal.sh      # (you, with sudo) rootfs with /usr/local/bin/op2-air-run + its job plist
    tools/xpcd-cache-add.py           # iOS 12 launchd loads jobs ONLY from xpcd_cache.dylib (section __xpcd_cache,
                                      # a bplist); a plist dropped in LaunchDaemons is ignored. The job is added there,
                                      # padded to the section's size, written in place (original: backup/rootfs/xpcd_cache.dylib.orig)
    output: /private/var/tmp/air on the phone (read from TWRP with tools/op2hfs.py); /dev/console does not reach ramoops
Kernel fixes this needed: MSM_PARAM_GPU_ID = 430 (was the chip id: Mesa refused the GPU); GEM_MADVISE (retained);
SUBMITQUEUE_QUERY (unsupported, quietly). SP/TP power (gpu_init bit 16) is now on by default and stable (POWER_STATUS
bit 20 set, full runs, G0/G1 still pass).
The job (com.op2.air-run) runs air_run once at every boot; with KeepAlive false it does not repeat.
Next (G4): more of what CA uses -- textures + samplers, blending, the uber shaders with the captured constants
(air_run takes function constants), larger targets; then G5 (CA through the GPU).

## G4 status (2026-11-11): CA's real uber pipelines draw on the A430; texture/varying/constant paths exact
air_run --batch (tools/mesa-op2/air-run-batch.patch: runs separated by --next, each its own process) from the
launchd job (tools/g4-air-run-job.plist, put into xpcd_cache by tools/xpcd-cache-add.py), 8 runs in one boot, every
one on FD430, 8 submits, no hang, no reset (logs/g4-air-run-1.txt). Inputs (air_run): every varying = (c,d,c,1) with
c = d = 0.375 (centre of texel 1,1 of 4x4); texel (x,y) of slot n = (100+x+1000n, 200+y, 300+x+y, 400); constant
buffers = bytes (i*7+13)&0xff; target r16g16b16a16_float.
    run                                            pixel (0,0)                        verdict
    clear_frag                                     0.000989 0.0119 0.130 1.554        EXACT (half4 of the buffer bytes)
    ca_uber_fragment_lp0_cp0_fo1 (backboardd fc)   101 201 302 400                    EXACT (texture sample, texel 1,1)
    ca_uber_fragment_lp0_cp0_fo0 (backboardd fc)   0.375 0.375 0.375 1                EXACT (a varying (c,d,c,1) passed through)
    radial_shading_frag                            0.000371 0.00447 0.0488 1.554      consistent: clear colour x 0.375 in rgb
    upsample_frag                                  102.06 202.88 305 399.75           plausible (multi-tap filter, half rounding)
    downsample_4_frag                              nan nan nan nan                    unverified (weights from random buffer bytes)
    ca_uber_fragment_lp0_cp0_fo0 (SpringBoard fc)  nan 1.226 -3.539 1                 unverified (same)
    path_blit_frag (2 RTs)                         0 0 0 0                            unverified
No independent reference yet: softpipe on the Mac asserts in nir_to_tgsi on these shaders (unknown opcode /
texture type), and macOS Metal on the M4 refuses iOS 12's .metallib format ("not supported on this platform or
built with an old version of the tools"). A reference would need the AIR re-targeted for macOS, or inputs chosen so
each result can be derived by hand (air_run --zero/--ones exist but are degenerate).
Not covered by air_run at all: blending, depth/stencil, MSAA, bigger targets, IOSurface-backed targets -- what G5
(CoreAnimation on the GPU) needs from the driver side.

## G4b (2026-11-11): blending and real targets exact -- and a memory-safety problem (logs/g4b-air-run-1.txt)
New air_run options (tools/mesa-op2/air-run-op2.patch): --blend (premultiplied source-over), --size WxH,
--rt bgra8, and a coverage line (how many pixels equal pixel 0,0; the last pixel). Source colour set with
--buffer 0@0=0038003400300038 (half4 0.5 0.25 0.125 0.5), destination cleared to 0 0 1 1. Every run as predicted:
    A  no blend, 4x4 half            0.5 0.25 0.125 0.5
    B  blend over blue               0.5 0.25 0.625 1
    C  B at 64x64                    4096/4096 pixels equal
    D  bgra8 64x64 (B G R A bytes)   32 64 128 128  (127.5 rounds to 128)
    E  bgra8 + blend 256x256         159 64 128 255, 65536/65536
    F  E at 1080x1920 (8 MB target)  159 64 128 255, 2073600/2073600
    G  CA ca_uber_fragment_lp0_cp0_fo0 (backboardd constants), bgra8 + blend, 1080x1920: 96 96 96 255, all pixels
**But after G the system fell over**: unrelated processes (suggestd, nsurlsessiond, calaccessd, cfprefsd, ...) crashed
with nonsense PCs (one is 0x9c0000009c00, the uber run's function-constant value 0x9c00 twice), then launchd ->
"initproc exited" panic. Most likely the GPU wrote outside its buffers. With the SMMU in BYPASS the GPU reaches
all of physical memory, so a stray write lands in whatever owns the page. Candidates: a draw writing past a buffer
(G has 2 render targets + more state than the clear_frag runs), or a buffer freed by Mesa's BO cache (gemClose) while
the GPU still used it, its pages then reused by other processes. The test job was taken out of launchd's cache again
(original xpcd_cache restored) so it does not run every boot.
Next: (1) find the stray write: rerun G alone with the kernel logging every BO's physical range and each submit's BO
list, and check gemClose waits for the BO's last fence; (2) put the GPU behind real SMMU translation (TZ already
configured the context banks; program CB0 with page tables of only the client's BOs) so a stray write faults
instead of corrupting memory -- needed before anything runs on the GPU in daily use.

## G4c (2026-11-11): the stray write located -- the full-size uber draw, and why it is dangerous
Kernel now instruments every gemNew buffer (kernel-backup/round6-gpu/g4-debug): a 16 KB guard band after the end
(filled 0xa5a5a5a5, checked at close) and a 48 MB quarantine of closed buffers (filled 0xc5c5c5c5, re-checked before
their memory is returned and at client exit). boot-arg gpu_held=0 disables the quarantine.
Evidence matrix (what crashes and what does not):
    uber ca_uber_fragment_lp0_cp0_fo0 @ 4x4            OK   (G4 batch, runs 6-8, exit 0)
    clear_frag @ 1080x1920 bgra8 + blend               OK   (G4b run F, all 2073600 px correct)
    uber ca_uber_fragment_lp0_cp0_fo0 @ 1080x1920      CRASH
So the trigger is the UBER SHADER AT FULL PANEL SIZE -- not size alone (clear_frag at the same size is fine) and not
the shader alone (it is fine at 4x4). The uber draw has 2 render targets, textures, several constant buffers.
G4c crash: panic "pmap_remove_pv: cpte ... does not match pv_h" at the moment of gemSubmit fence 2009 (3 cmds, 28 bos),
i.e. DURING the draw, before any buffer was closed -- a kernel page-table integrity failure, the signature of the
GPU having written over the kernel's own pmap metadata. (G4b was the same stray write landing in user pages instead:
other processes crashed with nonsense PCs, one = 0x9c0000009c00, the uber run's fc value 0x9c00 twice.)
The guard band and quarantine could not catch it: the overrun is large and lands far from the buffer (in kernel
structures), and the panic is mid-draw, before any close-time check runs. 16 KB of guard is far too little for an
overrun measured in megabytes.
CONCLUSION: a real GPU draw of this shape writes outside its buffers, and with the SMMU in BYPASS that write hits
whatever physical page it lands on. This cannot be made safe by watching from the kernel after the fact; the write
must be made to FAULT. So G4 stops here until the GPU is behind real SMMU translation (G4d / next): TZ already
configured the context banks (G0), so program CB0 with a page table that maps only the current client's BOs, point
the 3D core's context at it, and the same uber draw should fault (CB0 FSR/FAR naming the bad address) instead of
corrupting RAM -- which both contains the damage and tells us the exact address the draw got wrong.
Until then the air_run job stays OUT of launchd's cache (original xpcd_cache restored); op2-air-run is inert on disk.

## G4d (2026-11-11): GPU behind real SMMU translation; the stray write is Mesa fd4's private memory
Boot-arg **gpu_xl=1** (off by default; short name because the cmdline is at 248/250 bytes). Kernel:
kernel-backup/round6-gpu/g4d/ (before: g4d-pre/). Logs: logs/daily-20261111-202130.log (self-test),
logs/daily-20261111-203437.log (uber draw).

How (after Linux msm_iommu-v1.c __program_context, reference/kernel-oneplus-msm8994/drivers/iommu):
- CB0 = gfx3d_user (SID 0) is the only bank KGSL attaches on 8994 (priv_bank_table is v0-only); CB1 untouched.
  Only CB registers are written (CBAR/SMR are TZ's). TZ accepts them after restore_sec_cfg: SCTLR 0 -> 1 reads back.
- ARMv7 short descriptors, identity (IOVA = physical), **4 KB pages**: L1 16 KB, L2 tables (1 KB) carved from
  contiguous 16 KB chunks on demand, per-page refcount. Pages: Normal non-cacheable shareable, RW
  (TEX=001 C=0 B=0 AP=011) with TEX remap off (SCTLR.TRE=0), so no PRRR/NMRR. Walks non-cacheable; PTEs cleaned to PoC.
- SCTLR = M only: CFIE=0 (no IRQ; FSR/FAR latch anyway), CFRE=0, CFCFG=0 (terminate). TLBIALL + TLBSYNC after every
  map/unmap. Faults are polled in waitForFence and named against the client's buffers (describeFaults).
- Mapped: ring, fence page, every gemNew buffer (NOT its guard band), surfaces segment by segment, self-test IBs.
  Buffers are unmapped after their last fence. Recovery re-checks CB0 (it has always kept its programming).

Measured:
1. **ATS resets the SoC.** Any CPU-side translation (CB ATS1PR -> PAR), mapped address or not, resets the phone a
   few ms later, with no panic. Rounds g4d-1..8 were spent finding this, because IOLog's console buffer lost the tail
   of the log. SMMU bring-up lines now go straight into ramoops (OP2_SMMU_LOG -> op2_ramoops_ap_write). ATS is gone.
2. Boot self-test, CONTAINED: a mapped write lands; writes to an unmapped page and 0x40 past a mapped page fault
   with FAR = the exact address (FSYNR0 0x592 = write) and leave the victims untouched; after unmapping a page a write
   to it faults too (FAR = page base there) and is dropped; the GPU works afterwards. G0 1000/1000, G1 exact.
3. **After a faulting write the CP stalls** (the fence never retires) even with RAZ/WI: hang recovery restarts it.
4. **The G4c uber draw under translation: no crash.** ca_uber_fragment_lp0_cp0_fo0 at 1080x1920: 614 write faults,
   all dropped, GPU hang -> recovered, the process exits, the phone runs to its deadline. The faults start just past
   a group of small 16 KB buffers (handles 10-13, phys 0x19cd2000-0x19cee000) and climb to 0x5b640 past the last.

**Root cause of G4c:** the shader writes private memory (117 `stp`), and Mesa's fd4 never sizes private memory:
fd4_context.c allocates one fixed 8 KB fs_pvt/vs_pvt BO and fd4_emit.c writes the constant SP_FS_PVT_MEM_PARAM
0x08000001. Per-fiber private memory for a full-screen draw is far larger, so it runs past the BO. That is why 4x4
was fine (few fibers) and clear_frag at full size was fine (no stp). They are not spills: **117 store_scratch,
0 load_scratch**. The AIR front end (air_to_nir.c) puts allocas and private globals in scratch, and the CA uber
shaders write a 220-byte struct of function-constant values (hence 0x9c00 in G4b's corrupted PCs) that nothing
reads. nir_remove_dead_variables cannot drop it (the stores go through pointer casts), and ir3's own scratch
lowering is gen>=5 only.

## G4e (2026-11-11): fixed in Mesa; the full-size uber draw is correct with zero faults (logs/g4d-air-run-1.txt)
tools/mesa-op2/ir3-fd4-pvtmem.patch:
- ir3_nir.c: ir3_nir_remove_write_only_scratch(). If a shader has no load_scratch, every store_scratch is dead
  (scratch is per invocation): drop them, scratch_size = 0. It runs on every gen, not only has_pvtmem.
  CA pipelines (tools/ca-compile-captured.sh): 0 stp everywhere; backboardd fo0 140 -> 9 instructions,
  fo1 141 -> 11, vertex 90 -> 32; SpringBoard fo0 322 -> 182, vertex 85 -> 30.
- fd4_draw.c: a draw whose VS/FS still has pvtmem_size > 0 is skipped (logged once: "fd4: draw skipped: shader needs
  private memory"). A4xx private memory stays unsized (PARAM layout and per-SP fiber count unknown) until a shader
  really needs it.
On the phone (gpu_xl=1, round g4d-12, the G4c job: tools/g4c-air-run-job.plist):
    uber ca_uber_fragment_lp0_cp0_fo0 @ 1080x1920 bgra8+blend   2073600/2073600 px = 96 96 96 255 (= G4b run G), exit 0, twice
    clear_frag                                                  16/16 exact, exit 0, three times
    kernel: 0 SMMU faults, 0 recoveries after the boot self-test, the phone runs to its deadline
The new air_run was written in place over /usr/local/bin/op2-air-run, padded with zeros (previous one:
backup/rootfs/op2-air-run.g4c). The job is out of launchd's cache again (original xpcd_cache restored).

Next: (1) make gpu_xl=1 the default once a normal day's boot (metal=0, and then metal=2) is clean with it;
(2) the remaining G4 coverage (depth/stencil, MSAA, IOSurface-backed targets) and then G5; (3) Security, not covered by translation alone (automated review, 2026-11-11): any task can open OP2GPUUserClient
(gate it on an entitlement); one global table for all clients (needs per-client tables or a command parser);
GET_IOVA returns physical addresses; kOP2Surface binds any surface, including the 0x03400000 panel fallback,
with no ownership check.

## Blocked on
- Building Mesa: needs to be done on the Mac (`brew install meson ninja flex bison` + python mako).

## G2 results (2026-11-11): every CoreAnimation pipeline the phone asked for compiles for the A430
    backboardd  ca_uber_vertex_lp0            90 instr     ca_uber_fragment_lp0_cp0_fo0  140 instr
    backboardd  ca_uber_fragment_lp0_cp0_fo1  141 instr
    SpringBoard ca_uber_vertex_lp0            85 instr     ca_uber_fragment_lp0_cp0_fo0  322 instr
    sweep of all 62 functions with every constant 0: 61 compile (only path_cubic_vert fails)
How to reproduce:
    bash tools/mesa-host.sh           # Mesa 25.0.7 + hakios AIR front end + tools/mesa-op2/*.patch, all in out/mesa-host
    bash tools/ca-sweep.sh            # all functions, constants 0 -> logs/ca-sweep-430.txt
    bash tools/ca-compile-captured.sh # CA's real pipelines (logs/ca-fc-captured.txt) -> logs/ca-sweep-430-real/
Function-constant capture (logs/daily-20261111-061733.log): OP2Metal hooks the setters of
MTLFunctionConstantValuesInternal (the private class +alloc returns) and logs, per function, every value through
OP2GPU kOP2Log (now also accepting the line as 16 packed scalars). Boot with EXTRA="metal=2" (CA renders through
OP2Metal, so the screen stays blank that run); tools/fc-from-log.py turns the lines into AIR_FC strings. CA uses only
ca_uber_vertex_lp0 and ca_uber_fragment_lp0_cp0_fo0/fo1, with twelve int constants (indices 0..11).
OP2Metal must fit the rootfs file (71552 bytes, written in place by op2hfs): -Oz, dead_strip, no function starts,
no new imports. The capture build is on the phone now (harmless with metal=0); the original is backup/rootfs/OP2Metal.orig.

What iOS 12's AIR needed (tools/mesa-op2/air-ios12.patch, on hakios's front end):
1. **The static initialiser never ran.** iOS 12 puts it in section "__TEXT,__StaticInit,regular,pure_instructions"
   (iOS 16: "air.static_init"), so every variable it derives from function constants read as zero: wrong
   specialisation everywhere, and the uber shaders' texture phis (`has_image_0A ? tex : null`) could not fold
   ("a phi between pointers that name different places"). is_static_init() accepts both.
2. `[[color(e)]]` is a global set by that initialiser (color_attachment = (frag_buffer_params >> 14) & 3):
   eval_const_int() evaluates the chain at interface time (constants, function constants with the pipeline's
   values, globals through their single store, int binops/casts/compares/select, the predicate helper), for
   render-target inputs and outputs (outputs had silently collapsed onto target 0).
3. A texture whose function-constant predicate is false in this pipeline takes no binding and no slot
   (ca_uber_fragment_* declares [[texture(0..17)]] against 16 hardware slots).
tools/mesa-op2/ir3-cmdline-air-fc.patch: AIR_FC="index=value,..." for ir3_compiler.
Still open: path_cubic_vert -- ir3 assert (ubo->idx > 0) in ir3_get_shared_driver_ubo on a430, a530 and a630 alike
(the binning variant reusing a driver UBO; probably the command-line tool, not the GPU). Not used by the pipelines
CA asked for in this capture. The sweep's first 57/62 (before fix 1) compiled with every derived variable 0.
Next (G3): fd4 sysmem draw of a compiled ir3 program into an RGBA8 target.

## G5a (2026-11-11): what CoreAnimation asks of Metal (trace), and the driver design
OP2Metal now sends its log lines to the kernel log (op2_log -> kOP2Log -> ramoops, "M <process>: ...",
deduplicated per process with digits dropped; NSLog never reached ramoops). Every selector it does not implement
is logged by the forwarder. The G2 function-constant capture was removed to make room (71472 of 71552 bytes).
Trace boot: EXTRA="metal=2", logs/daily-20261111-212016.log.

What backboardd and SpringBoard use (and nothing else in a full boot):
- device: one command queue (SpringBoard: two); newFunction ca_uber_vertex_lp0 / ca_uber_fragment_lp0_cp0_fo0 with
  function constants; ~65 render pipeline states made up front in backboardd, 8 in SpringBoard
- render targets: IOSurface textures, backboardd 1242x2208 format 80 (BGRA8Unorm) x2 (double buffered);
  SpringBoard 162x162 and 2592x2592 format 80, 256x1 format 30 (gradients)
- textures: replaceRegion:mipmapLevel:(slice:)withBytes:bytesPerRow:(bytesPerImage:), storageMode, setLabel:
- render encoder: setViewport:, setScissorRect:, setRenderPipelineState:, setVertexBuffer:offset:atIndex:,
  setVertexBufferOffset:atIndex:, setFragmentTexture:atIndex:, setFragmentSamplerState:atIndex:,
  setFragmentBuffer:offset:atIndex:, setFragmentBufferOffset:atIndex:,
  drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:, endEncoding
- device (SpringBoard): newTextureLayoutWithDescriptor:isHeapOrBufferBacked:
- NOT used: drawables, presentDrawable, blit encoders, compute. CA renders into IOSurfaces and swaps them through
  the framebuffer itself (our kernel FB path already does that).

Design:
- /usr/local/lib/op2mtl.dylib: Mesa fd4 + the AIR front end, linked as air_run is (~9-10 MB), behind a C API:
  device; pipeline (vertex + fragment function, constants, colour format, blend) compiled through AIR->NIR->ir3;
  buffers (BOs, CPU-mapped); textures incl. IOSurfaces (kernel kOP2Surface import, mapped per segment under the
  SMMU); replace_region; samplers; render pass begin (load/clear) / state / indexed draw / end + fence.
  air_run already does every one of these: it is the starting point.
- OP2Metal stays a thin shim: dlopen the library, implement exactly the selectors above on it, keep today's stubs
  when the library is absent (an all-zero slot) or fails.
- Every G5 boot with gpu_xl=1. The cmdline has room for metal=2 or gpu_xl=1, not both: flip gpu_xl's default or
  free bytes before G5 boots.
- Room on the rootfs: tools/rootfs_g5.py (run by tools/rootfs-journal.sh, needs sudo, ONCE) makes a 24 MiB zero slot
  for op2mtl.dylib and gives OP2Metal a 1 MiB slot; after that both are replaced in place by tools/op2hfs.py.

## G5a-1 (2026-11-11): op2mtl, the driver core: CA's own pipeline draws on the phone (logs/g5a-op2mtl-2.txt)
userland/op2mtl/: op2mtl.h (the C API, Metal's own enum values), op2mtl.c (air_run's proven draw path behind it),
op2mtl_test.c. Built by tools/mesa-ios.sh (sources copied into src/compiler/air every run; targets in
tools/mesa-op2/op2mtl-meson.patch, present only when the sources are, so the host build is unaffected):
out/mesa-ios/out/op2mtl.dylib (8.4 MB, install name /usr/local/lib/op2mtl.dylib, 35 exported op2mtl_* calls) and
op2mtl_test (52 KB, links it). On the phone: the dylib in its 24 MiB slot; the test in /usr/local/bin/op2-air-run's
slot (that slot's previous content: backup/rootfs/op2-air-run.g5a-slot), job tools/g5a-op2mtl-test-job.plist
(output /var/tmp/op2mtl); the job is OUT of launchd's cache again.
What it does: device (fd4 screen, FD_MESA_DEBUG=sysmem); metallib libraries; pipelines from a vertex + fragment
function with function constants, the vertex descriptor turned into Gallium vertex elements by the front end's
driver_location, blend per attachment; buffers whose `contents` is the BO's own CPU mapping (an UNSYNCHRONIZED
map kept for the buffer's life: freedreno has no persistent maps); textures and IOSurface textures
(resource_from_handle(WINSYS_HANDLE_TYPE_FD, IOSurface ID): freedreno's dmabuf import IS the kOP2Surface import on
this port); samplers incl. the shader's inline ones; render passes with load/clear, Metal's viewport (negative y,
clip_halfz) and scissor; draws and indexed draws; commit = flush + fence, wait = fence_finish. A draw whose
pipeline reads a binding that is not set is refused by name (that is how the first run said what CA's vertex
function reads).
Measured (gpu_xl=1, round g5a-op2mtl-2): CA's ca_uber_vertex_lp0 + ca_uber_fragment_lp0_cp0_fo0 with backboardd's
constants, 7 stage-in attributes from one interleaved buffer, an identity transform in vertex constant buffer 1,
an indexed quad over the middle half of a 1080x1920 BGRA8 target: fence completed, 512416 of 512416 pixels inside
drawn, 0 of 1549184 outside, 0 SMMU faults, no hang. CA's vertex function reads constant buffers 1, 2, 3, 5; its
fragment function 0, 1, 2, 3, 5 and samplers 0..5. Geometry is verified; the COLOUR is not (the fragment
constants are a byte pattern; the inside varies by quadrant): CA's real buffer contents come with the shim.
Next: the OP2Metal shim (dlopen op2mtl.dylib; MTLBuffer/MTLTexture/IOSurface texture/pipeline/encoder selectors of
the G5a trace onto the API; log CA's pipeline descriptors and bindings), then metal=2 boots.

## G5b (2026-11-12): the OP2Metal shim on op2mtl; CoreAnimation composites on the Adreno
OP2Metal maps CA's Metal calls onto op2mtl (buffers, IOSurface / bytes-no-copy / ASTC textures, samplers, function
constants via hooked MTLFunctionConstantValues setters, pipelines, render passes, synchronous commit + wait). Levels:
`metal=2` = only backboardd gets the Metal device (SpringBoard and apps draw with the CPU); `metal=3` = every process.
Fourteen rounds, logs `logs/g5b-round*.out` and the daily logs of the same times; every fix and its peer reference is
in `docs/PEER-FINDINGS.md`. Kernel snapshots `kernel-backup/round6-gpu/g5b-*` (g5b-13 = state of commit 758c0a6).
Measured: backboardd ~120-280 pipelines at ~0.7 s each, ~7 ms GPU per commit, 0 failed draws, no SMMU faults.
Phone (user's eyes): `metal=3` composites the home screen with right icons/colours but pages don't snap and no app
launches; `metal=2` launches Settings (rendered mostly right, a yellow wash and cyan bands remain), shows the iOS 12
wallpaper, but SpringBoard's own layers are missing / text in black boxes; blur materials fail.
After 758c0a6: op2mtl widens 16-bit varyings to 32 bits (CA's colours are half4; both peers needed this;
`OP2MTL_F16VARY=1` = old behaviour) and lowers [[point_coord]] to a PNTC input + per-vertex point size (blur shaders).
Review handover: `docs/FABLE-HANDOVER-PROMPT.md`.
