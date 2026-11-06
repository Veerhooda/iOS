# QuartzCore "Software renderer! Simulating crash!" — disassembly artifact (iOS 12.5.7 16H81)

Source: dyld_shared_cache_arm64 from the iPhone7,1 IPSW root filesystem
(`out/038-87140-050.dmg`, mounted read-only). Cache base 0x180000000 verified
from the dyld_v1 header mappings (map0 __TEXT 0x180000000 size 0x2effc000).

Disassembled with `otool -tV` via Mach-O slicing (same method as
`tools/disas_cache.py`). Strings:

- `0x1854efe02`: `CoreAnimation: Software renderer! Simulating crash!\n`
- `0x1854efe37`: `CoreAnimation: Failed to initialize Metal or OpenGLES contexts!`

## Branch site (QuartzCore render-server init)

```
0x185448034  bl   0x1852fff98          ; GPU/context check -> w0
0x185448038  cbz  w0, 0x185448050      ; w0==0 -> else branch (still fails below)
0x18544803c  adrp x1, 0x1854ef000
0x185448040  add  x1, x1, #0xe02       ; x1 = "CoreAnimation: Software renderer! Simulating crash!"
0x185448044  orr  w0, wzr, #0x2
0x185448048  bl   0x18530000c          ; log it
0x18544804c  b    0x185448090
0x185448050  ... else: probe global + call 0x180b03b84 ...
0x185448090  adrp x0, 0x1854ef000
0x185448094  add  x0, x0, #0xe37       ; x0 = "CoreAnimation: Failed to initialize Metal or OpenGLES contexts!"
0x185448098  bl   0x1853ab2a0          ; report it (RETURNS normally; verified epilogue)
0x18544809c  ... normal continuation (vtable call [x8,#0xa8], flag OR, ret) ...
```

## Check function 0x1852fff98 (returns nonzero = software renderer)

Reads globals at 0x1ba754000+0xfa8/0xfa0 (cached GPU state / function pointer),
calls through when present, else ORs two pointers at 0x1b8617000+0xb50/0xb48
and returns nonzero if either is set. Full listing in session notes; key point:
the decision is driven by GPU-init globals populated earlier (IOKit
`AGXAccelerator` matching + driver-bundle probing), not by the framebuffer.

## On-device correlation (logs/run61_ramoops.txt)

- Line ~1460: `IOK backboardd: matching1 ... AGXAccelerator ... -> none`
- Line 1462: `CoreAnimation: Software renderer! Simulating crash!` (once; pid 56 survives)
- Lines 1430/1476: `PUIProgressWindow ... _renderWithIOSurface 0`
- After: level-2999 emergency overlay, all-zero Surface 3 swaps.

## Implication

Both the w0!=0 and w0==0 paths converge on the "Failed to initialize Metal or
OpenGLES contexts!" report, so single-branch patching cannot cleanly bypass it.
The supported remediation order is:

1. Remove the OP2-only divergence first (restore GPU bundles; ios-sweet leaves
   them intact and reaches the home screen with CPU rendering).
2. If the abort persists with bundles intact, the iOS 12 render path needs a
   GPU context it can never get on this SoC, and phase 2 is either a fake
   accelerator service or deeper shared-cache surgery — both need iterative
   device testing, not more static guessing.
