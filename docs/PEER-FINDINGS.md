# What other iOS-on-Android GPU projects learned (read 2026-11-12)

Projects that run CoreAnimation on a non-Apple GPU through a Metal driver over Mesa. All of them use the same AIR
front end lineage as ours (hakios, MIT). Their documents are copied under `reference/peers/` (read-only; ideas are
re-implemented here, nothing is copied without a licence check).

| Project | What | Document |
|---|---|---|
| ry-diffusion/Chlorophyll (+ Bergamota) | iOS 18, Poco F1, Adreno 630: Metal over freedreno, CA composites the home screen and Control Center | `chlorophyll-FIXES.md`, `bergamota-FIXES_KERNEL_SYSTEM.md` |
| limonetexd/ios-sweet-gpu (`gpu` branch) | iOS 16, Redmi Note 10 Pro, Adreno 618: lock/home screen, Control Centre, Settings on the GPU | `sweet-METAL_FINDINGS.md` |
| oosexgang/hakios-xnu-kernel | iOS 16, Adreno 660: the AIR front end's origin; its own direct-PM4 Metal driver | `hakios-docs-reference-metal.md`, `hakios-docs-metal-plan.md`, `hakios-hakios-metal-README.md` |
| Nik300/viva-gpu | iOS 16 on Mali-G57 (Panfrost + AIR->NIR) | not read yet |
| assergames78-glitch/ios12-tablet | iOS 12 XNU on an MT6737T tablet (CPU drawing) | no GPU notes |

## Their fixes, against what we have

| Fix (source) | Their symptom | Ours | Status here |
|---|---|---|---|
| Link vertex outputs to fragment inputs by AIR name, not order (Chlorophyll fb5d79f) | colours from the wrong slot | orange second hand drawn yellow-green (g5b-7 photo) | DONE g5b-11: `tools/mesa-op2/air-varying-link-name.patch` + op2mtl `link_varyings` |
| 16-bit varyings cross VS->FS as 32-bit (Chlorophyll afb033a, sweet 0006) | diagonal garbage, saturated blocks; matched CA's CPU render after | CA's lph shaders, half colours | TODO if colours stay wrong |
| Lines and points 1 px wide (Chlorophyll 175571a) | every mask empty (CA draws mask coverage as lines) | not seen yet | DONE g5b-11 (rasterizer) |
| `flatshade_first` (Chlorophyll 130eca2) | flat colours swapped between a quad's triangles | not seen yet | DONE g5b-11 |
| ASTC LDR textures, uploads count rows of blocks (Chlorophyll fce9d47) | wallpaper / snapshots missing | wallpaper black | DONE g5b-9/10 |
| newBufferWithBytesNoCopy / newTextureWithBytesNoCopy: copy at each GPU use (Chlorophyll 175571a) | layers missing | icons/wallpaper black | DONE g5b-8 (textures); buffers: copied at bind already |
| hakios: userptr import (wire the caller's pages, map into the GPU VA, clean the CPU cache at each submit) | - | the right way for no-copy textures | LATER: needs a real GPU VA space (IOVA) |
| hakios: IOSurfaces cleaned out of the CPU cache before GPU use | - | CPU-drawn surfaces sampled by the GPU can read stale RAM | TODO |
| Big GPU buffers refused after a while (Bergamota 0237: VA page tables ran out) | flat wallpaper, missing backdrop | 2592x2592 refused (contiguous RAM) | DONE g5b-10: boot-time contiguous pool |
| Kernel GEM handle limit -> `fd_bo_new` NULL -> crash in shader upload (Chlorophyll b927867) | backboardd crash every few minutes | same crash, round g5b-3 | DONE (2048 handles); their purge-and-retry patch also worth taking |
| Refused draw/clear is skipped, the frame goes on (Chlorophyll 2cb58f9) | black/grey wallpaper | we skip already | OK |
| Load actions once per pass; DontCare targets start cleared (sweet 0003, Chlorophyll 198d8c3) | home screen black except changed regions | - | check |
| Swap held until the GPU fence passes (Bergamota 0229) | half-drawn frames scanned out | commits are synchronous for now | OK until async |
| Framebuffer-fetch barrier under batch reordering; fetch draws in sysmem (Chlorophyll 0237bf4) | half of every blur missing on a diagonal | we are sysmem | watch when blurs work |
| Scissor honoured; corner patches drawn 1 px outside it (Chlorophyll 004f9fa) | black corner wedges on rounded layers | - | check |
| `load_point_coord` unhandled in ir3 | - | blur shaders fail to compile (g5b-7) | TODO: lower point coord in the front end / op2mtl |
| CoreAnimation debug environment (sweet section 4): CA_DISABLE_DIRTY_REGIONS, CA_DISABLE_ASYNCHRONOUS, CA_ENABLE_MPS_BLURS, ... | A/B switches without code | - | use for diagnosis |
| GPU scan-out copy, async commits (Bergamota 0240/0241) | 20 -> 38 fps | performance | LATER |
| Texture pool, BO cache skip-busy, bounded constant uploads (Chlorophyll) | 30 -> 41 fps | performance | LATER |
| Colour judgements from photos need the user's eyes (sweet 10.2: a "blue tint" was the camera) | - | - | keep in mind |
