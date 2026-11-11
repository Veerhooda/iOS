# op2gpu

The Adreno 430's command processor from userland: buffers, submissions and fences through the kernel's `OP2GPU`
user client (Linux msm-DRM request numbers and structures), plus fills and copies done by the command processor
alone. This is G1 of [docs/GPU-PLAN.md](../../docs/GPU-PLAN.md).

| File | What it is |
|---|---|
| `include/op2gpu/gpu.hpp`, `src/gpu.cpp` | buffers (GEM objects), submit, wait, cpu_prep; adapted from hakios-metal (MIT, `LICENSE-hakios`) |
| `include/op2gpu/pm4.hpp` | A4xx PM4 type-0/type-3 packets — only the ones measured on the phone |
| `include/op2gpu/blit.hpp`, `src/blit.cpp` | fill (inline `CP_MEM_WRITE`) and copy (`CP_MEM_TO_MEM`, 64-bit where aligned) |
| `src/fake_gpu.cpp` | a strict in-process GPU for the tests: refuses opcodes not measured on the A430 |
| `src/checks.cpp` | the checks, shared by the host tests and the phone probe |
| `src/iokit_transport.cpp` | the `OP2GPU` service through IOKit |
| `tools/probe.cpp` | the checks on the phone, results written to the kernel log |

```bash
make test     # host: the checks against the fake GPU, plus mutations it must refuse
make ios      # out/ios/op2gpu-probe for iOS 12 (Xcode 10.3 toolchain on PATH)
```

On the phone (2026-11-11): fills of 60 (length, offset) pairs byte exact, a 500 000-byte copy byte exact with the
same hash as the host, 40 batches in order, 1 MB fill at 591 MB/s. `CP_MEM_TO_MEM` copies are correct but slow
(~17 MB/s); bulk copies belong to the 2D/3D engine.
