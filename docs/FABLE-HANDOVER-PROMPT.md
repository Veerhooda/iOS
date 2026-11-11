# Handover prompt: OnePlus 2 iOS port, GPU (Metal) bring-up review

You are taking over, for a review-and-plan pass, a hobby port of Apple's iOS 12.5.7 userland (iPhone 6 Plus,
N56 / iPhone7,1, build 16H81) running on Apple's open-source XNU kernel (xnu-4903.270.47), booted on a OnePlus 2
(Snapdragon 810 / MSM8994, Adreno 430 GPU, 4 GB RAM). The phone runs the real, unmodified iOS userland: launchd,
backboardd, SpringBoard, Settings. Everything below the userland is ours: a boot shim, the kernel port, our own drivers
(eMMC, display, touch, IOSurface, GPU), and now a Metal driver so CoreAnimation can composite on the GPU.

The person you work for runs every boot on the real phone, films the screen, and executes what you plan. Another model
(me) did the GPU work so far, in long debugging loops: one boot, read the log, guess, fix, boot again. We are worried the
approach itself may be wrong in places, that small mistakes have piled up, and that we are spending days guessing at
things that someone with a clearer view could get right the first time. **Your job is the clearer view.** Do most of the
thinking: audit, correct, and give us a plan we can execute step by step with little guessing.

Work in the repository at `/Users/veerhooda/oneplus2` (the person will give you access). Read before you judge; quote
file:line for every claim about our code.

---

## 1. Read these first (in order)

1. `CLAUDE.md`: hard safety rules (below) and current state.
2. `docs/AUDIT-2026-11-11.md`: an earlier audit of what the previous agent got wrong.
3. `docs/GPU-PLAN.md`: GPU history G0 to G5a-1 (kernel driver, SMMU, Mesa, first draws).
4. `docs/PEER-FINDINGS.md` and `reference/peers/*.md`: what three other projects doing the SAME thing learned
   (Chlorophyll/Bergamota: iOS 18 on Adreno 630; ios-sweet-gpu: iOS 16 on Adreno 618; hakios: iOS 16 on Adreno 660, the
   origin of our AIR shader front end). Their bug lists are the best map we have.
5. The code:
   - `userland/OP2Metal/OP2Metal.m`: the Metal driver plug-in (an Objective-C bundle Metal.framework loads; classes
     built at run time on Apple's private `_MTLDevice` etc.; ~2700 lines). It maps Metal calls onto op2mtl.
   - `userland/op2mtl/op2mtl.{c,h}`: the driver core, a C API over Mesa 25.0.7's freedreno (fd4) Gallium driver plus
     the AIR -> NIR front end (from hakios). Built by `tools/mesa-ios.sh`; Mesa patches in `tools/mesa-op2/`.
   - Kernel (gitignored, under `xnu/xnu-4903.270.47/iokit/Kernel/arm/`): `OP2GPU.cpp/.h` (GPU: power, firmware, ring,
     fences, SMMU, the msm-DRM-like user client), `OP2Surface.cpp` (our IOSurface implementation),
     `OP2Framebuffer.cpp` (IOMobileFramebuffer stand-in; copies/downsamples frames to the panel with the CPU).
     Backups of every version changed in this session: `kernel-backup/round6-gpu/g5b-*`.
6. Logs: `logs/daily-2026111{1,2}-*.log` (ramoops kernel log of each boot; lines `OP2 GPU user: M <proc>: ...` are
   from OP2Metal). The rounds of this session: `logs/g5b-round{1..13}.out` and the daily logs of the same times.

## 2. Hard rules (do not violate, do not plan anything that does)

- Volume/Power buttons are broken. Never rely on them.
- Never `fastboot flash` anything. Boot only with `fastboot boot out/xnu-boot.img` (RAM boot).
- Keep the BCB cookie armed so every reset lands in TWRP (`tools/boot-daily.sh` does it).
- Every boot image must have `op2_deadline` (the only escape from a hang; the hardware watchdog strands the phone).
- Do not touch CPU voltage beyond the stock values in `op2_cpu_boost`.
- Kernel source under `xnu/` is gitignored: back up before editing (`kernel-backup/`).
- NEVER use the SMMU's ATS (address translation) registers: they reset the SoC.
- The boot cmdline is limited to 250 bytes (`tools/build-daily.sh` checks). It is nearly full.
- Files on the phone's rootfs are rewritten in place by `tools/op2hfs.py` (from TWRP); it cannot grow a file. Slots:
  `/usr/local/lib/op2mtl.dylib` 24 MiB, `OP2Metal` 1 MiB.

## 3. How a test round works (what "executing" costs us)

`EXTRA="metal=2" bash tools/round.sh`: builds the kernel, RAM-boots, the phone runs until `op2_deadline` (900 s) resets
it into TWRP, the ramoops log is pulled into `logs/daily-*.log`. One round is about 20 minutes. Installing a new
op2mtl.dylib takes ~3 more (adb, in place). We cannot attach a debugger; there is no serial. Userland stdout/NSLog do
not reach the log: OP2Metal writes through a kernel log call (`kOP2Log`), deduplicated, capped per process. A
stuck-thread watchdog in OP2Metal logs backtraces (image+offset; symbolicate with `atos -o out/*.unstripped -l 0`).
Unstripped builds of every round are kept in `out/`. **Design every experiment so ONE boot answers as much as possible.**

## 4. Where we are (state on 2026-11-12, round 13)

Verified on the phone:
- Kernel GPU driver: microcode runs, fences, hang recovery, SMMU translation for the GPU with containment self-tests
  (stray writes fault and are dropped). Translation is on whenever `metal>=2`.
- Mesa fd4 + the AIR front end compile CoreAnimation's real uber shaders (`ca_uber_vertex_lp0`,
  `ca_uber_fragment_lp0_cp0_fo0`, specialised by function constants) and draw them correctly in a standalone test.
- With `metal=3` (every process on the GPU): backboardd composites the home screen on the GPU with correct icons, text
  and colours, live (pages follow the finger). But pages do not snap after release and NO app launches (no app process
  is even spawned), while CPU-only boots launch Settings.
- With `metal=2` (new this session: only backboardd gets a Metal device; SpringBoard and apps draw with the CPU and
  backboardd composites everything on the GPU): Settings launches from a tap and renders mostly correctly. The default
  iOS 12 wallpaper draws (ASTC texture). Round 13's log is `logs/daily-20261112-045033.log` (the newest; nobody has read it yet:
  start there, especially the `X ...` lines, one per distinct texture binding). Remaining faults, from photos:
  - Settings' background has a yellow wash at the bottom and faint cyan vertical bands (should be flat light grey).
  - Rainbow speckles on the Airplane Mode switch.
  - The home screen shows the wallpaper but SpringBoard's own layers (clock, icons, dock) are missing; the status bar
    items ("Searching", battery) sit in opaque black boxes.
  - Blur materials (dock, folders) fail: ir3 rejects `load_point_coord` in the blur shaders (they fail cleanly now).
  - Three pipelines arrive with a nil vertex function (`ca_uber_fragment_lp0_cp0_fo1`, `downsample_4_frag`,
    `narrow_blur_23_frag`); cause unknown.
- Performance: each pipeline takes ~0.7 s to compile (backboardd makes ~120-280 at start: minutes of compiling),
  commits are synchronous (CPU waits for the GPU each frame), no-copy textures are re-uploaded each frame, the panel
  copy is a 5 ms CPU downsample (1242x2208 -> 1080x1920) plus a full cache clean of the source each frame.

Fixed in this session, in order (each one a full round; see `docs/PEER-FINDINGS.md` for the peer cross-reference):
attribute the vertex descriptor leaves out reads 0; unbound constant buffers and textures read zeros; kernel race on
a global list (panic in `IOUserClient::free`); per-client GPU buffer table 256 -> 2048 (backboardd crashed in
`ir3_shader_variant` on a NULL BO); ir3 compile errors spun forever (release-build `unreachable()`), now they fail;
`iosurface:NULL` render targets; non-contiguous IOSurfaces given to the GPU as if contiguous (now allocated contiguous
under 4 GB when `metal>=2`); `newTextureWithBytesNoCopy` was unimplemented (all layer images black); ASTC formats;
a 192 MB boot-time contiguous pool for big GPU buffers; the panel copy read stale CPU cache lines (GPU frames never
reached the panel); varyings linked by AIR name instead of order (colours from the wrong slot); 1 px lines/points;
`flatshade_first`; SpringBoard/apps off the GPU (`metal=2`).

## 5. Our design, and where we doubt it (audit these first)

Be blunt. If a choice is wrong, say so and say what to do instead, with the cost.

1. **GPU addressing.** The GPU sees physical addresses (an identity SMMU map, 4 KB pages, 32-bit). Every GPU buffer
   must be physically contiguous and below 4 GB; we added a boot-time pool and contiguous IOSurfaces to cope. The peers
   use a real GPU virtual address space (IOVA) with per-client tables, which also allows "userptr" imports of another
   process's pages (how hakios serves CA's no-copy textures) and fixes the security problems in item 8. Should we stop
   patching the identity scheme and build IOVA now? What is the smallest correct design for the A4xx + MSM8994 SMMU
   (short-descriptor tables, context bank 0, TZ owns the SMMU's global registers)?
2. **Synchronous everything.** `commit` flushes and waits for the GPU; completion handlers run after that. Bergamota
   went from ~20 to ~38 fps with async commits and a GPU scan-out copy, but needed a swap gate (hold a swap until its
   fence passes). What is the right order to introduce async without new corruption?
3. **The panel path.** The kernel downsamples 1242x2208 to the 1080x1920 panel on the CPU and cleans the cache every
   frame. Would presenting iOS a 1080x1920 display (no downsample, direct scan-out) or a GPU blit be simpler and faster?
   What does iOS 12's CA need to accept a different panel size (MobileGestalt, device tree)?
4. **Coherency model.** GPU writes RAM directly; the CPU caches. We now clean+invalidate a surface before the panel copy
   and clean on IOSurface unlock (writer side). GPU buffers are mapped write-combined to userland. Is that complete?
   Where else can stale lines bite (texture uploads through Mesa staging, CPU readback, held/recycled buffers)?
5. **Metal in SpringBoard and apps (`metal=3`).** Why did giving SpringBoard a Metal device stop page snapping and app
   launches entirely? Our guesses: our synchronous commit or the pass lock blocking SpringBoard's main thread; completion
   handlers run on the wrong thread or at the wrong time; something CA expects of `presentDrawable`/drawables; slow
   pipeline compiles blocking the main thread. How would you find out in ONE boot?
6. **The Metal object model.** OP2Metal subclasses Apple's private `_MTLDevice`, `_MTLCommandBuffer` etc. at run time
   and forwards everything it does not implement to a stub that returns zeros. The peers note AGX's device has ~490
   selectors and ours ~80. Which missing selectors are likely to matter for CA on iOS 12 (blit encoder, texture views,
   generateMipmaps, `newTextureLayoutWithDescriptor:isHeapOrBufferBacked:`, `setLabel:`)? Is subclassing `_MTLDevice`
   right, or should we implement the protocols on plain NSObject subclasses?
7. **The shader path.** AIR (Apple's LLVM bitcode) -> NIR via hakios's front end -> ir3 for the A430 (gen 4). The peers
   all target a6xx. A4xx-specific risks: half-precision (16-bit) varyings (peers widen them to 32 bit; we have not),
   `load_point_coord` (unhandled), framebuffer fetch (CA's uber shaders use it; how does fd4 do it in sysmem?),
   private memory (we drop write-only scratch with a patch), compile time (~0.7 s per pipeline: shader cache? variant
   reuse? compile lazily?).
8. **Security (known, deferred).** Any process can open the GPU user client and submit command buffers that reach
   physical memory; buffer addresses are physical; IOSurfaces can be looked up by guessable ID; recycled screen memory is
   not zeroed. We want the GPU stack hardened before anyone relies on this phone: entitlement/root gate, per-client
   address spaces, CP protected registers, IOVAs instead of physical addresses, surface ownership. Fold this into the
   architecture plan rather than leaving it as a separate afterthought; tell us what order makes sense.

## 6. What we need from you (deliverables)

Write each as a section of one document, `docs/FABLE-REVIEW.md`, so we can execute from it.

A. **Audit.** For each of the 8 items above and anything else you find in the code: right / wrong / risky, with
   file:line evidence and the concrete fix. List the things most likely to be silently wrong (they cost us the most):
   enum mappings (pixel/vertex formats, blend factors, write masks, sampler modes), viewport and y-flip, clip space,
   premultiplied alpha, sRGB, texture row strides and alignment, IOSurface strides/offsets/plane handling, buffer
   offsets and constant buffer sizes, index buffer offsets, function-constant bit widths, resource lifetimes (what CA
   frees while the GPU still uses it), threading (which Metal calls can come from which threads), error paths that
   return nil to CA.

B. **Diagnosis of the open faults** (section 4), most likely cause first, and the ONE-boot experiment that tells the
   candidates apart. Include what to log and where.

C. **A test harness so we stop debugging through photos.** We need on-device verification that does not depend on the
   camera (ios-sweet-gpu found a "colour bug" that was the camera). Specify:
   - a self-test program (like Chlorophyll's `bgmtl_quad_probe` / `carenderer_probe` or sweet's 19-test `metaltest`)
     that runs through the public Metal API in a launchd job, renders known scenes (including CA's own uber shaders with
     the constants backboardd uses), reads them back and checks pixels, and writes PASS/FAIL to the kernel log;
   - a framebuffer capture we can pull from TWRP (the kernel copies a frame to a file or reserved memory; a tool turns it
     into a PNG), so we compare what the panel shows against a CPU-rendered reference;
   - a CA-level reference: the same layer tree rendered by CA on the CPU vs on our GPU, diffed (Chlorophyll does this
     with `CARenderer`).
   Tell us what to write, in what order, and which existing pieces to reuse (`userland/op2mtl/op2mtl_test.c`,
   `userland/OP2Metal/metal_test.m`, `tools/g5a-op2mtl-test-job.plist`, `tools/op2hfs.py`).

D. **A plan, in phases, with exit criteria.** From here to: (1) a usable phone with the compositor on the GPU (home
   screen, apps, Control Center, blur materials correct; no regressions vs CPU boots); (2) smooth (target fps,
   measured how); (3) Metal for SpringBoard and apps; (4) hardened. For each step: files to change, the experiment, the
   expected log lines, what "done" means, and the rollback. Order the steps so each boot answers the most and no step
   depends on guesses. Mark which peer fix each step corresponds to.

E. **Reuse instead of reinvent.** What can we take from the peers (Chlorophyll, ios-sweet-gpu, hakios, viva-gpu) and
   from Mesa/turnip/freedreno itself, with the licence of each (MIT / APSL / GPL / unknown) and what to rewrite instead
   of copy. Also: is there an Apple behaviour reference we should use (class-dumps of iOS 12 Metal/QuartzCore,
   environment variables like `CA_DISABLE_DIRTY_REGIONS`, `CA_ENABLE_MPS_BLURS`; the peers list them)?

F. **Things we have not thought of.** Anything that will bite later: memory pressure and jetsam with Mesa in
   backboardd, the hardware watchdog during long compiles, power/clocks/thermal for the GPU (GPU frequency scaling,
   GDSC), suspend/resume and display-off, multiple displays/rotation, video (YCbCr IOSurfaces: RG8 plane 1, format 30),
   CoreImage/MPS users (Siri orb, wallpaper effects), 120-pipeline startup cost, shader cache on disk, and how all this
   behaves after hours rather than 15 minutes.

G. **A short "do not" list.** Mistakes you see us making or about to make.

## 7. How to work

- Prefer evidence over plausibility: read the code and logs, cite them; mark every claim as verified (seen in code/log)
  or inferred.
- When you are unsure, say what single boot would settle it instead of guessing.
- Keep each experiment small enough to undo; keep the CPU boot (`metal` unset) working at all times.
- Do not copy code from projects whose licence is unclear; describe the idea and we re-implement.
- Write for an executor who is not a GPU expert: exact files, exact functions, exact expected log lines.
- Respect section 2. Nothing you plan may flash a partition, depend on the hardware buttons, or drop `op2_deadline`.
