# GPU bring-up review (2026-11-12, after round 15)

Answer to `docs/FABLE-HANDOVER-PROMPT.md`. Every claim is marked **[V]** verified (seen in code, a log, or a host run
made for this review) or **[I]** inferred. Kernel line numbers are in the committed snapshot
`kernel-backup/round6-gpu/g5b-13/` (the live tree under `xnu/` is gitignored and has moved a few lines since).

## 0. The five findings that matter most

1. **Framebuffer fetch is not implemented, and half of CoreAnimation's shaders use it.** [V] Of the 162 distinct
   function-constant sets backboardd asked for in rounds 13 and 15, **86 compile to a shader that reads the
   framebuffer** (`isamm.a.s2en...`, ir3's `txf_ms_fb`), 74 do not, 2 failed on `load_point_coord` (fixed in round
   15). Mesa's A4xx driver has no code that binds the framebuffer for that read (`fb_read` appears only under
   `a6xx/`). Those draws read an unbound texture slot. This is the most likely cause of the black boxes around text,
   the missing clock/icons under `metal=2`, and the speckles. Fix: section B1. Check:
   `grep -c isam /tmp/fsv/*.txt` after re-running the sweep in section C1.
2. **(Corrected after rounds fable-16..18.)** The kernel cleans the CPU cache over every buffer of every submission
   (`OP2GPU.cpp:2420-2426`). This review first called that pure waste, on the grounds that GPU buffers are mapped
   write-combined into userland (`:2529`). **Measured: it is not waste.** With the clean skipped for owned buffers,
   the GPU hung 18 times in a boot with framebuffer fetch OFF (round fable-18, `metal=12`): the userland mappings
   evidently are cached in practice, and the clean is what makes Mesa's writes visible. The full clean is the
   default again (`gpu_clean=1`). Making it cheaper needs knowing which buffers the CPU wrote since the last
   submission (track `MSM_GEM_CPU_PREP`/`CPU_FINI`, or make the mappings really write-combined and verify), not
   skipping it.
3. **We debug on the phone what the host can answer in a minute.** [V] `out/mesa-host/.../ir3_compiler -g 430` and
   `air2nir` already compile CA's real shaders with the phone's real constants. Finding 1, the `load_point_coord`
   failure and the off-by-one varying slots are all visible there with no boot. Every round since G5a could have started
   with a sweep. Method fix: section C1.
4. **Nothing captures what the GPU drew.** Every judgement so far came from photos. Section C2 specifies a frame dump
   that works on CPU boots and GPU boots alike, so the two can be diffed on the Mac.
5. **`metal=3` (Metal inside SpringBoard) is a different problem from compositing and should wait.** [V] In rounds 11
   and 12 SpringBoard was never stuck in a GPU call (no `W` watchdog line) and committed exactly one command buffer;
   what it did do is ask for render targets with a NULL IOSurface (162x162, 2592x2592) and call
   `newTextureLayoutWithDescriptor:isHeapOrBufferBacked:`, which we answer with nil. `metal=2` avoids all of it.

Round 15 (both new op2mtl fixes: 16-bit varyings widened, point coordinate) has **not been looked at by a person**.
Its log shows no failure. Look at the phone before trusting or discarding either fix.

---

## A. Audit

### A1. GPU addressing (identity map): works, but stop adding to it
- [V] The GPU's addresses are physical addresses: `MSM_INFO_GET_IOVA` returns `b->phys_addr` (`OP2GPU.cpp:2289`),
  indirect buffers are submitted by physical address (`:2452`), and `mapRange` / `mapDescriptor` (`:812`, `:880`) enter
  identity pages. Consequences already paid for: contiguous IOSurfaces, the 192 MB boot pool, the below-4-GB mask.
- Verdict: **acceptable for phase 1, wrong for the end state.** Move to real IOVAs in phase 4 (section D), not now:
  the open faults are not caused by it, and changing addressing while colours are wrong mixes two unknowns.
- Smallest correct design when you do it (no userland change, Mesa treats the value as opaque):
  - One shared GPU address space (this is also what Linux's msm driver does on A3xx to A5xx; per-process tables only
    exist on A6xx). IOVA window e.g. `0x1000_0000..0xF000_0000`, a range allocator (reuse `op2_pool_alloc`'s free-list
    code), `mapDescriptor` walks physical segments and enters them **consecutively** at the IOVA. Return the IOVA
    from `MSM_INFO_GET_IOVA`.
  - ARMv7 short-descriptor small pages output 32-bit physical addresses, so pages must still be below 4 GB (keep the
    mask), but they no longer need to be contiguous: the pool and the contiguous-surface allocation can then go.
  - Keep TLBIALL + TLBSYNC, but batch it: once per submission that follows a map/unmap, not once per map.
  - Userptr imports (hakios's answer to no-copy textures) become possible only for pages below 4 GB; keep the copy
    path as the fallback.
- Per-client tables are not available cheaply (one context bank, TZ owns the rest; switching TTBR0 per submission
  needs an idle GPU). Security therefore comes from gating who may open the client (A8), not from isolation.

### A2. Synchronous commits: right for now
- [V] `gpuCommit` (`OP2Metal.m:2321`) flushes and waits up to 2 s; handlers run after. Simple and correct; it also means
  CA cannot free or reuse anything the GPU still reads, which hides every lifetime bug (A7).
- Go async only after the picture is right and the frame dump exists. Order: (1) remove the per-submit cache clean
  (A4); (2) report "scheduled" at submit and "completed" from a waiter thread; (3) the kernel holds a swap until the
  fence of the frame's last submission passes (Bergamota 0229); (4) objects named by an encoder are retained until that
  fence (sweet 0005/0007). Steps 3 and 4 before step 2 ships.

### A3. The panel path
- [V] The CPU copy drops 3 of every 23 columns and rows by nearest pick (`OP2Framebuffer.cpp:1300-1334`, "sp[22] is
  skipped"): text is slightly broken on the glass even when the GPU frame is perfect. Plus a clean+invalidate of the
  11 MB source every frame (`:1275-1279`).
- Do not change the display size iOS sees: an iPhone 6 Plus really renders 1242x2208 and scales to 1080x1920; other
  sizes mean a different device identity and layout. Instead (phase 2): let the GPU do the scale. In OP2Metal, after a
  commit that drew into the screen IOSurface, draw one linear-filtered quad from it into a 1080x1920 surface from the
  carveout (the kernel already scans those out directly: `OP2Surface.cpp` "MDP5 Direct Scanout Optimization",
  `OP2Framebuffer.cpp:1257`) and hand that surface to the framebuffer. Removes ~5 ms CPU and the cache pass, and
  filters properly.

### A4. Coherency model: one hole; the per-submit clean is NEEDED
- **[V, measured]** `gemSubmit` step 1 (`OP2GPU.cpp:2420-2426`) cleans every BO named in a submission. Skipping it
  for owned buffers hung the GPU (round fable-18). It stays. To make it cheaper later: find out why the mappings
  that `clientMemoryForType` asks to be write-combined (`:2529`) still hold dirty lines (check the mapping's cache
  mode in `mapClientMemory64`; Mesa may also write through a second, cached mapping), or clean only buffers the CPU
  mapped for writing since their last submission. Measure with the "512 submissions: cache cleaned over ... KB" line
  that round fable-16 added.
- **Hole [I]:** a CPU reader of a GPU-written surface. Only the panel copy invalidates (`OP2Framebuffer.cpp:1279`).
  A process that locks a surface the GPU rendered (snapshots for the app switcher, screenshots) reads stale lines
  through its cached mapping. Fix: in `kOP2SurfaceLock` (`OP2Surface.cpp:933`), `op2_civac` the surface if the GPU has
  imported it (set a flag in the `kOP2Surface` import, `OP2GPU.cpp:2702`).
- Texture uploads through Mesa go through the same BO mappings, so they depend on the same per-submit clean.

### A5. `metal=3`: see B5. Not a commit/lock problem [V].

### A6. The Metal object model: keep it
- Subclassing `_MTLDevice`/`_MTLCommandBuffer` at run time is what the working peers do; it gives ~340 default
  selectors for free. Keep it.
- The real risk is the forwarder (`OP2Metal.m:418`): an unimplemented selector returns zeros **silently after its
  first log line**, and the blit encoder class implements only buffer copy/fill (`:907`), so texture copies and
  `generateMipmapsForTexture:` are dropped without a trace beyond one `unimpl` line. Change the forwarder to count
  calls per selector and print the counts with the stats line.
- Selectors to implement, in this order, each only when a log shows it called: `newTextureLayoutWithDescriptor:
  isHeapOrBufferBacked:` (B5), blit `copyFromTexture:...toTexture:...`, `generateMipmapsForTexture:` (Chlorophyll
  deac59c), `newTextureViewWithPixelFormat:`, `setLabel:` (no-op), GPUStartTime/GPUEndTime (return the wait times).

### A7. Things that are silently wrong today
| What | Where | Status |
|---|---|---|
| Framebuffer fetch | no code | **Wrong** [V], B1 |
| `bytesPerPixel`: A8Unorm (1) counted as 4 bytes, RGBA16Float (115) as 4, RGBA32Float (125) as 4 | `OP2Metal.m:1053` | **Wrong** [V]. Used for no-copy row size (`:1340`) and the CPU fallback of replace/getBytes: an A8 no-copy texture would get 4x the row stride. Fix: 1 -> 1, 10/11 -> 1, 20/25/30/31 -> 2, 115 -> 8, 125 -> 16, 65 -> 4, 55 -> 4 |
| MTLVertexFormat 51 mapped to R16_FLOAT | `op2mtl.c:156` | **Wrong** [V]: 51 is UShortNormalized, Half is 53. Signed formats (4-6, 10-12, 16-18, 22-24, 32-35, 46, 48, 50, 52) are missing: add them |
| Pixel formats | `op2mtl.c:76-124` | Right as far as they go [V against the 12.4 SDK]. Missing and plausible for CA: 11 (R8 sRGB), 20 (R16Unorm), 40/43 (B5G6R5, BGR5A1), 60, 110, 94, 550-555 (XR; answer NO to `supportsExtendedXR10Formats`, Chlorophyll 2e65262) |
| Blend factors/ops, write mask, primitive, index type, load action, sampler address/filter | `op2mtl.c:161-212` | Right [V]. Unknown factor falls back to ONE silently (`:174`): make it a logged failure |
| Viewport, y-flip, clip space | `op2mtl.c` `op2mtl_set_viewport`, rasterizer `:454-463` | Right [V]: negative y scale, `clip_halfz`, top-left memory order; geometry was pixel-exact in G5a-1 |
| Point coordinate orientation | `ir3_gallium.h:56` forces flip mode for PNTC, `fd4_program.c:505-512` emits "1 - T" | **Risky** [I]: we already flip y in the viewport, so a point sprite's T is probably upside down for Metal. Blur passes draw points. Test in the self-test (C3) before believing any blur |
| No-copy textures re-uploaded at first bind after every commit | `OP2Metal.m:2191` | **Wasteful** [V]: the ASTC wallpaper is 6.7 MB per frame. Chlorophyll observed CA makes a new texture when a backing store changes. Replace with: upload at creation, then re-upload only if a cheap hash (first, middle, last 4 KB) changed; log how often it does |
| `setVertexBytes`/no-copy buffers make a new GPU buffer per call | `op2mtl.c:1124` | **Wasteful** [V]: each is a kernel allocation, an SMMU map and a TLB flush. Use one upload ring (Gallium's `u_upload_mgr`) |
| Constant buffer size | `bind_for_draw`, `op2mtl.c:1180` ff. | Up to 64 KB bound per buffer per draw; fine on A4xx (pointer, not copy) [I] |
| Lifetimes | encoder keeps unretained pointers (`OP2Metal.m` `struct op2_encoder`), op2mtl keeps raw pointers | Safe only because commit is synchronous. Before async: retain until the fence |
| Threading | one device lock in op2mtl, one pass lock in the shim | Right for one compositor. `fail()` writes one global error string (`op2mtl.c:63`): two threads can swap messages; make it thread-local when convenient |
| nil returned to CA | pipelines always return an object; textures return a CPU stub | Right (matches Chlorophyll 2cb58f9) |
| Three pipelines with a nil vertex function | log `G pipeline #3/#14/#15` | Unknown, harmless so far [V]: later pipelines with the same fragment functions arrive complete (`downsample_blur_vert + downsample_4_frag`, round 7). Log `[pd label]` and the class of `pd.vertexFunction` once and move on |
| Pipeline cost | `op2mtl.c:687-688` | Each pipeline re-parses the bitcode and compiles both stages; 280 pipelines but 162 distinct constant sets and the vertex function has far fewer. Cache compiled stages by (function, constants, linked slots); B6 |

### A8. Security: what is true and the order to fix it
- [V] Anyone can open the client (`newUserClient`, `OP2GPU.cpp:1863` and `:1896`); addresses are physical (`:2289`); surfaces can be
  looked up by ID (`OP2Surface.cpp:1033`); carveout surfaces are not zeroed (`:885`).
- Order: (1) now, cheap, no design impact: zero carveout memory on allocation; (2) phase 4a: gate `newUserClient` to
  root or an allowlist of process names taken from the task (backboardd, and SpringBoard/apps only at `metal=3`), and
  program the A4xx `CP_PROTECT` registers so command streams cannot write RBBM/CP/VBIF/SMMU registers (Linux's
  `a4xx_protect` table is the reference); refuse `gemSubmit` when translation is off; (3) phase 4b: IOVAs (A1);
  (4) surface ownership: lookups by ID only for surfaces the caller holds or that are marked global.
- Per-process GPU isolation is not achievable on this hardware without a TTBR switch per submission. Say so in the
  README instead of implying it.

---

## B. Diagnosis of the open faults, and the experiments

### B1. Black boxes around text, missing SpringBoard layers, speckles: framebuffer fetch (most likely) [V cause exists, I that it explains all three]
CA's uber fragment function reads the destination for its programmable blend modes. **Update (rounds fable-16..19):** the shadow copy below goes through Mesa's 3D blitter mid-pass; the default is now
to sample the target IN PLACE after `texture_barrier` (a flush on freedreno), which sysmem rendering allows;
`metal=42` selects the copy. The original plan, kept for reference:
1. After `create_fs_state` in `op2mtl_pipeline_new`, ask ir3 what it compiled:
   `struct ir3_shader *sh = ir3_get_shader(p->fs)` (header `ir3/ir3_gallium.h`, it waits for the async compile), take
   the first non-binning variant `v`; `p->fb_read = v->fb_read; p->fb_slot = v->num_samp - 1` (ir3 appends the fetch as
   the last sampler: `ir3_compiler_nir.c:3899-3908`). Log `pipeline: ... reads the framebuffer at sampler N` once per
   distinct N.
2. Device: one shadow texture the size and format of the current colour attachment 0 (recreated when the target's size
   or format changes), plus a nearest sampler.
3. In `bind_for_draw` (`:1180`), when `p->fb_read`: `resource_copy_region` from the attachment into the shadow (the
   scissor rectangle if a scissor is set, else the whole target), then put the shadow's view at `views[p->fb_slot]` and
   the nearest sampler at the same index, raising the counts passed to `set_sampler_views` / `bind_sampler_states`.
   Freedreno's batch tracking orders the copy after the earlier draws and before this one.
4. Count fetch draws and copied pixels in the stats line. If a frame has many full-target copies, compute the draw's
   bounding box from its vertices (positions are float4 at offset 0, stride 64, transform in vertex buffer 1) and copy
   only that.
A/B without a second boot image: make `metal=` carry flags, level = value % 10, flags = value / 10 (the kernel only
tests `>= 2`): `metal=2` normal, `metal=12` = fetch emulation off. Costs one cmdline byte (248 of 250).
Expected: status-bar text without boxes, lock-screen clock visible, home-screen icons over the wallpaper.
If the picture is unchanged, the frame dump (C2) plus the per-draw trace (pipeline serial, fb_read, blend) of one frame
names the draw that is wrong.

### B2. Settings: yellow wash, cyan bands
Candidates: (a) 16-bit varyings (fixed in round 15, unverified); (b) fetch (B1); (c) vertex colour `color1` read from
uninitialised vertex bytes: CA's vertices carry heap garbage at offsets 32 and 48 (round 8 trace) and the vertex
function's `color1` attribute is predicated on the constant `vtx_has_color1` (`air2nir --interface` prints
`fc=vtx_has_color1`); if our translation keeps reading it when the constant says no, garbage reaches the colour.
One boot: round 15's build already tests (a). For (c), the host answers: run `air2nir` on `ca_uber_vertex_lp0` with a
logged constant set and check whether attribute 5 survives in the output.

### B3. Blur materials
Compile was fixed in round 15 (`reads [[point_coord]]: a PNTC input`, round 15 log). Still open: point-coordinate
orientation (A7), per-vertex point size (set in round 15), and fetch (Chlorophyll's blur chain ends in a fetch draw).
Do B1 first, then judge blurs with the frame dump.

### B4. Three pipelines with a nil vertex function: see A7. Log once, no boot of its own.

### B5. `metal=3`: pages do not snap, apps do not launch
What the logs say [V]: no GPU call was stuck; SpringBoard made one commit of 5 draws into a 162x162 target; it created
162x162 and 2592x2592 render targets with `iosurface:NULL`; `newTextureLayoutWithDescriptor:isHeapOrBufferBacked:`
returned nil. [I] SpringBoard uses Metal to render things it then reads back or shares (icons, wallpaper), gets
nothing usable, and its main thread ends up waiting on a result that never arrives. One boot, `metal=3`, with:
- in the forwarder and in `deviceNewTextureIOSurface`'s NULL branch: a backtrace of the caller (the watchdog's
  `addrText` already prints image+offset; add `dli_sname` so shared-cache frames have names);
- the watchdog extended to sample SpringBoard's **main thread** whenever its run loop has not turned for 2 s (a
  `CFRunLoopObserver` stamping a time; the watchdog thread compares), regardless of CPU use;
- at start, log the methods of protocol/class `MTLTextureLayout` (`objc_getProtocol`, `protocol_copyMethodDescriptionList`).
That names the API SpringBoard waits on. Do this after phase 1; `metal=2` is the shipping shape until then.

### B6. Minutes of shader compiling at start
Measure first: split the `P` line's time into translate (AIR->NIR) and `create_*_state`. Then, in order: (1) cache the
compiled vertex and fragment state by (function name, constants, the fragment's linked slot map) and share between
pipelines that differ only in blend state; (2) compile lazily at the first `setRenderPipelineState:` instead of at
creation (retain the two function objects in the pipeline; CA warms up far more variants than a frame uses); (3) a
disk cache keyed by the same hash (`nir_serialize`) in `/var/tmp` if (1)+(2) leave more than a few seconds.

---

## C. Test harness

### C1. Host sweep before every boot (exists; 1 minute)
`tools/ca-compile-captured.sh` runs every captured constant set through `ir3_compiler -g 430`. Extend it:
- take constants from the new log lines (`M backboardd: P<n> ok ... fc 0=...,11=...`): a 10-line `tools/fc-from-log.py`
  update (`grep -E "P[0-9]+ (ok|FAILED)"`, `sed` the `fc` field into `AIR_FC=0=0x..,`), both functions;
- report per variant: compiled or the error line, instruction count, **fetches** (`grep -c isam`), samplers;
- fail the script if a variant errors.
Rule: a Mesa or op2mtl change is not booted until the sweep is clean. The host build asserts (it is a debug build), so
it also catches what the phone's release build would run through.

### C2. Frame dump (write this first; 1 kernel change, 1 host script)
In `OP2Framebuffer.cpp`, in the copy path after the source surface is known (`:1269` ff.): at presents 3, 30 and then
at most once every 20 s when a new frame arrived, max 8 per boot, wake a kernel thread that writes the **source
surface** (1242x2208 BGRA, after the cache invalidate) to `/private/var/tmp/op2fb-<n>.bgra` with `vnode_open` /
`vn_rdwr`. Never write from the swap path itself. Log `OP2 FB DUMP: n=<n> present=<p> crc=<crc32>`.
On the Mac: `tools/op2fb-pull.sh` reads the files with `tools/op2hfs.py read` from TWRP and converts to PNG
(`python3 -c` with zlib, or `ffmpeg -f rawvideo -pix_fmt bgra -s 1242x2208`).
Because it sits in the kernel's display path it works identically on CPU boots: **a CPU boot gives the reference
image, a `metal=2` boot the test image, and `compare` or a 20-line script gives the diff.** The CRC in the log also
tells, without pulling anything, whether two boots drew the same frame.

### C3. On-device Metal self-test (`userland/OP2Metal/mtl_selftest.m`, run by the existing `com.op2.air-run` job)
Public Metal API only, so it tests the shim and op2mtl together. Each test prints one line `T<nn> PASS|FAIL name got
want` to stdout (the job's `StandardOutPath`, read back with `op2hfs.py`) and to the kernel log. Start from
`userland/op2mtl/op2mtl_test.c` (scene and checks) and `tools/g5a-op2mtl-test-job.plist`. Let the shim show the
device to this program at `metal>=1` (`shownToThisProcess`, `OP2Metal.m:2568`: add its name next to `metal_test`).
Tests, in this order (each is a bug we had or a peer had):
1 device and families; 2 buffer round trip; 3 texture replace/getBytes round trip for formats 80, 70, 1, 10, 30, 115;
4 clear + readback through an IOSurface (CPU reads the surface after `IOSurfaceLock`: this is the A4 hole);
5 CA's uber pair, opaque variant, quad, exact colour (vertex colour 0.25/0.5/0.75 must come out as that);
6 same with a blended variant over a known background (premultiplied source-over, exact result);
7 **a fetch variant** over a known background (B1; pick a constant set whose host compile shows `isam`);
8 A8 text mask tinted by vertex colour; 9 no-copy BGRA texture and no-copy ASTC texture (a 8x8 block pattern);
10 scissor; 11 two draws, second overlapping the first, fetch variant (ordering of the shadow copy);
12 point sprite with a gradient from `point_coord` (orientation); 13 lines 1 px; 14 flat shading provoking vertex;
15 100 passes (leaks, fences); 16 completion handler order. Tests 5 to 8 take their expected colours from the same
scene rendered by softpipe on the host (`air_run` on the Mac), not from hand-computed values.

### C4. CA reference
After C2 and C3: a small program that builds a layer tree, renders it with `CARenderer` into an IOSurface once with the
Metal device and once with CA's software path, and diffs (Chlorophyll's `bgmtl_carenderer_probe` is the model; write
ours). Lower priority: C2 already gives whole-screen CPU-vs-GPU diffs.

---

## D. Plan

Each step is one boot unless marked "host". Keep `metal` unset working: nothing below touches the CPU path except C2,
which is read-only there.

### Phase 1: the compositor is right (`metal=2`)
| # | Step | Files | Expected in the log | Done when | Peer |
|---|---|---|---|---|---|
| 1.0 | Look at round 15 on the phone (no build) | - | - | You know whether widening / point coord changed the picture | - |
| 1.1 | Host sweep script (C1) | `tools/` | "162 variants: 160 ok ... 86 fetch" | script exists, runs clean | - |
| 1.2 | Frame dump (C2) + a CPU boot for references | `OP2Framebuffer.cpp`, `tools/op2fb-pull.sh` | `OP2 FB DUMP: n=1 ...` | PNGs of lock screen and home screen from a CPU boot on the Mac | - |
| 1.3 | Framebuffer fetch (B1) + `bytesPerPixel` + vertex format 51 (the per-submit clean STAYS: rounds fable-16..18) | `op2mtl.c`, `OP2Metal.m`, `OP2GPU.cpp` | `reads the framebuffer at sampler N`; stats line with fetch draws; cleaned-bytes counter near 0 | dump of lock screen and home screen differs from the CPU reference only in blur areas | Chlorophyll 004f9fa (sysmem fetch) |
| 1.4 | Self-test (C3), tests 1-11 | `mtl_selftest.m`, job plist | `T07 PASS ...` | all pass, or each FAIL has a named cause | sweet metaltest |
| 1.5 | Blurs: tests 12-13, then the dock/Control Center in the dump | `op2mtl.c` | - | dock and folders match the CPU reference closely | Chlorophyll blur fixes |
| 1.6 | CPU readers of GPU surfaces (A4 hole); unknown blend factor = failure; forwarder call counts | `OP2Surface.cpp`, `op2mtl.c`, `OP2Metal.m` | no new `unimpl` with a count above 0 that matters | app switcher snapshots are not stale | - |
Exit: lock screen, home screen, Settings, Control Center, a folder: dumps match the CPU boot except for sub-pixel
differences; 15 minutes of use with 0 failed draws, 0 SMMU faults, no backboardd restart.
Rollback at every step: boot without `metal=`; the previous `op2mtl.dylib` / `OP2Metal` builds are in `out/`.

### Phase 2: smooth
Measure with numbers already in the log (`S` lines: commits, GPU wait) plus two new ones: encode time per commit and
frames per second over a scripted swipe (`op2_autoinput=2` if it can drag; otherwise a fixed 10 s manual swipe).
Order by expected gain per risk: (2.1) pipeline cache + lazy compile (B6): start-up from minutes to seconds;
(2.2) no-copy texture hash, upload ring (A7); (2.3) GPU scale-out to a carveout surface (A3); (2.4) texture pool and
BO cache fixes (Chlorophyll `fd-bo-cache-skip-busy`, 15 s retention; purge-and-retry on a refused allocation);
(2.5) async commits with the swap gate and retains (A2). Exit: 30+ fps in the swipe test, no start-up stall above 10 s.

### Phase 3: Metal in SpringBoard and apps (`metal=3`)
B5's diagnostic boot first. Then implement what it names (likely `MTLTextureLayout`, IOSurface-less targets shared
with the render server, readback/blit paths). Exit: with `metal=3`, pages snap, Settings launches, self-test passes
inside a second process while backboardd composites.

### Phase 4: hardened
4a gate + CP_PROTECT + zeroed carveout + refuse submit without translation; 4b IOVAs (A1), then delete the pool and
the contiguous-surface allocation; 4c surface ownership. Exit: a test program run as `mobile` cannot open the client;
a submit that writes a protected register faults and is logged; nothing outside the GPU driver sees a physical address.

---

## E. Reuse

| Source | Licence | Take | How |
|---|---|---|---|
| Mesa 25.0.7 (freedreno, ir3, u_upload_mgr, u_blitter, nir_serialize) | MIT | everything needed for B1, B6, A7 | already in tree |
| hakios AIR front end (`reference/hakios-xnu-kernel/tools/mesa/air`) | MIT | already ours; track upstream commits (e.g. 3b9db78 nearest-variant selection is for iOS 16's 451 named variants: **not applicable** to iOS 12, whose uber shaders are specialised by constants) | patch files |
| hakios kernel parts (`xnu/files`, `drivers/`) | APSL-2.0 | ideas only (userptr import, cleaning surfaces before GPU use) | re-implement |
| Chlorophyll | README says MIT, GitHub shows no licence | ideas and test designs: BO purge-and-retry, cache skip-busy, texture pool, the quad probe and CARenderer probe, the trace switches | re-implement until the licence is confirmed with the author |
| ios-sweet-gpu | fork of an MIT project; patches unreviewed | the CA environment-variable list, the selector gap count, the 19-test list | ideas |
| Linux msm (`a4xx_gpu.c` protect table, a4xx register init) | GPL-2.0 or MIT (dual in places) | register values are facts; write our own table | re-implement, cite the file |
Apple behaviour references worth using: CA's environment variables (sweet section 4), set for backboardd through its
launchd job in `xpcd_cache` (`tools/xpcd-cache-add.py`): `CA_DISABLE_DIRTY_REGIONS=1` (full redraws: simpler frames to
diff), `CA_DISABLE_ASYNCHRONOUS=1`, `CA_PRINT_SHADERS`. Class-dump iOS 12's Metal.framework once for
`MTLTextureLayout` and the `_MTLDevice` default list (B5, A6).

---

## F. What will bite later

- **Memory.** Mesa in backboardd: BO caches, a shadow target per size (B1), 280 pipelines. Jetsam limits apply to
  backboardd. Log `phys_footprint` of backboardd with the stats line; cap Mesa's BO cache.
- **Pool exhaustion.** 192 MB, first fit, 64 KB granules: fragmentation over hours. Log free/largest-free with the
  stats line; phase 4b removes the pool.
- **Buffer table.** 2048 handles per client is a constant; the peers hit their 4096 with caches on. Add the
  purge-and-retry on refusal before it happens.
- **Display off / auto-lock.** CA stops committing; anything that counts "time since last commit" (our watchdog's
  no-commit sample, future watchdogs) must start at the first commit and pause when the display is off (Chlorophyll
  e5f9f73 made this mistake).
- **GPU power.** The GPU runs at one clock with no idle gating; check `OP2 TOP`/temperature over an hour before long
  sessions. No frequency scaling is needed for correctness.
- **Hang recovery under load.** Recovery was tested on an idle GPU. With async commits, a hang loses in-flight frames:
  the swap gate must time out (Bergamota: 100 ms cap) so the screen cannot freeze on a lost fence.
- **Formats not seen yet.** Video and camera surfaces are two-plane YCbCr (plane 1 as RG8, format 30, through
  `newTextureWithDescriptor:iosurface:plane:` with plane 1: we import plane 0 only, `OP2Metal.m` IOSurface branch).
  XR 10-bit formats if CA is ever told the display is wide colour: keep `supportsExtendedXR10Formats` NO.
- **CoreImage / MPS clients** (Siri orb, some wallpapers) only matter at `metal=3`.
- **Tile overshoot.** Bergamota 0234: the Adreno writes whole tiles past a surface whose height is not a tile
  multiple. We render in sysmem bypass, which should not, but an SMMU fault just past a surface's end would be this.
- **Hours, not minutes.** After phase 1, one 2-hour run (`DEADLINE=7200`) with the stats line every minute: handles,
  pool, footprint, commits, failed draws.
- **The log budget.** `op2_trace` stops at 700 lines per process (`OP2Metal.m:87`). Per-frame traces must be
  switch-gated (the `metal=` flags of B1), not always on.

---

## G. Do not

1. Do not boot a shader or driver change the host sweep has not passed (C1).
2. Do not judge colour or "is it drawn" from a photo once the frame dump exists (C2); do not chase a tint without
   asking the person to look at the glass.
3. Do not change two unknowns in one boot unless a flag in `metal=` can separate them.
4. Do not rebuild `out/mesa-ios/out/op2mtl.dylib` while an install is running (round 14 was lost to that): install from
   a frozen copy.
5. Do not start IOVA, async commits or `metal=3` before phase 1's exit: each hides or multiplies picture bugs.
6. Do not add more boot-args: 247 of 250 bytes are used. Encode switches in `metal=`.
7. Do not trust "pixel-correct" claims in the older docs beyond what they tested: G4e and G5a-1 each verified one
   constant set of 162.
8. Do not copy from Chlorophyll or the sweet fork until their licences are confirmed; do not import hakios kernel
   files (APSL) into this tree.
9. Do not treat unbound-reads-zero as the end of the matter: each `is not bound: it reads zeros` line is a place where
   our translation reads something Apple's compiler removed. They are a to-do list for the front end (dead reads
   behind function constants), and B2(c) may be one of them.

---

## H. Results after the review (rounds fable-16 .. fable-23, 2026-11-12)

| Round | Change | Result (log + frame captures, no photos) |
|---|---|---|
| 16-17 | fetch via a shadow copy (Mesa's 3D blitter mid-pass) + kernel per-submit clean skipped for owned BOs | GPU hang nearly every commit |
| 18 | `metal=12` (fetch off), same kernel | still 18 hangs: **the per-submit clean is needed** (A4 corrected); restored as default |
| 19 | fetch reads the target in place after `texture_barrier` | 0 hangs; status bar still in black boxes |
| 20 | captures by time | first exact GPU frames: wallpaper pixel-correct; status-bar items in opaque black boxes; no clock |
| 21 | trace upload alpha + pipeline per binding | CA's status-bar images arrive transparent (corner `00000000`); they are drawn by programmable-blend pipelines (hardware blend OFF, the shader fetches the destination). ir3 emits the fetch as `isamm` (multisample texel fetch), which reads zeros on the A430 |
| 22 | op2mtl `lower_fb_fetch`: each fetch of attachment 0 -> plain `txf` (ISAML) at frag coord from an explicit slot; other attachments read 0 | **black boxes gone**: status bar ("Searching", time, battery) composites correctly over the wallpaper; 0 hangs, 0 failed draws |
| 23 | whole-frame draw trace (`F` lines) | the home-screen frame has exactly 5 draws: wallpaper + 4 status-bar items. **No draw for icons or dock exists**: backboardd's CA leaves them out before calling the driver (no texture created, nothing refused, nothing skipped) |

Open, in order:
1. **Icons and dock under `metal=2`** are culled by CA's Metal render path, not lost in the driver. Candidates [I]: the
   icon pages are offscreen/rasterised layers or portals that the Metal renderer handles differently from the CPU
   renderer; or the icon images reach the server in a form the Metal path cannot make a texture of. Next boot:
   class-dump-guided logging in backboardd of what CA asks the device before it would draw (every selector, counted:
   make the forwarder count, A6), plus `CA_DISABLE_*` A/B switches set in backboardd's job (sweet section 4:
   `CA_DISABLE_HIDDEN_CULLING`, `CA_DISABLE_OCCLUSION_CULLING`, `CA_DISABLE_FILTER_MERGING`) to see which one brings
   the icons back.
2. The 3rd 1242x2208 IOSurface texture backboardd imports (id 1004 in round 23): what it is (a snapshot? the
   rasterised icon page?).
3. Everything in section D phase 1 after 1.3.

## I. Rounds fable-24 .. fable-27: the home screen on the GPU

| Round | Change | Result |
|---|---|---|
| 24 | every draw traced from boot (`metal=2`) | every frame of the boot is the same 5 draws (wallpaper + status bar): under `metal=2` CA's GPU renderer never draws SpringBoard's main window (icons, dock, lock-screen clock) |
| 25 | `metal=3` with today's fixes | backboardd draws the lock-screen content (clock and date text, a 550x135 offscreen pass); the 192 MB pool runs out (28 MB and 12 MB refused) and SpringBoard crashes after its 2592x2592 pass begins |
| 26 | pool 384 MB under `metal=3`; refusal log with pool free/largest | no refusals; **lock screen fully correct** (`docs/img-fable26-lockscreen.png`); backboardd and SpringBoard crash in `stateOf` (`_op2State` ivar lookup) on a freed texture: the round-23 frame trace kept texture pointers across frames |
| 27 | frame trace off and reset per pass | **home screen fully correct and stable for 15 min** (`docs/img-fable27-homescreen.png`): icons, labels, live Clock icon, wallpaper, page dots, status bar, dock icons; 3900+ commits, 0 hangs, 0 failed draws, 0 crashes, ~2.8 ms GPU per commit |

So **`metal=3` is the mode to develop** (phase 3 of section D turned out to be required for phase 1). Open:
1. The dock's material is a flat grey-blue band, not frosted glass: the blur passes (compile fixed in round 15) are not
   yet verified (orientation of `point_coord`, the downsample chain).
2. Page snapping and app launch under `metal=3` (failed in round 11, before every fix of rounds 13-27): needs a person
   at the phone.
3. Captures in the logs come from backboardd only; the kernel-side dump (C2) is still to do for CPU-vs-GPU diffs.
