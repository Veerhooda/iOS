"""CoreVideo NULL-dict guard (hunkCV): 1-slot patch for the
SpringBoard SEGV at CoreVideo+0x18fe0 -> CFDictionary/SetApplyFunction.

Crash (run169/loop-22, SB pid41 @min38): CoreVideo's function at static
0x184210edc takes a collection in x1 (saved to x20) and passes it
unchecked to CFApplyFunction:

  0x184210fd8: mov x0, x20        ; x0 = dict (NULL: no-GPU object never built)
  0x184210fdc: bl  0x180e25c48    ; CF apply-fn, faults: ldr x8,[x0], x0=0

The out-param [sp,#0x10] is pre-zeroed and the bl's return value is
ignored: the fall-through path already bails gracefully on a NULL result
(ldr x20,[sp,#0x10]; cbz x20 -> epilogue). Skipping the bl on NULL-dict
is exactly "apply to zero elements" = no-op.

Patch (1 slot, no trampoline):
  0x184210fd8: mov x0,x20 (0xaa1403e0) -> cbz x20,#8 (0xb4000054)

Usage: ./hunkCV.py <dyld_shared_cache_arm64>   (patches in place; back it up!)
"""
import struct
import sys

VA = 0x184210FD8
FILE_OFF = VA - 0x180000000  # __TEXT mapping is file-first in dyld_v1 cache
EXPECT = 0xAA1403E0  # mov x0, x20
PATCH = 0xB4000054   # cbz x20, #8 (skip bl -> graceful bail)
BL_EXPECT = 0x9730531B  # bl 0x180e25c48 (sanity: call site intact)


def main():
    p = sys.argv[1]
    with open(p, "r+b") as f:
        f.seek(FILE_OFF)
        w = struct.unpack("<I", f.read(4))[0]
        if w == PATCH:
            print("hunkCV already applied")
            return
        assert w == EXPECT, "site mismatch: %s (cache differs?)" % hex(w)
        f.seek(FILE_OFF + 4)
        b = struct.unpack("<I", f.read(4))[0]
        assert b == BL_EXPECT, "call mismatch: %s" % hex(b)
        f.seek(FILE_OFF)
        f.write(struct.pack("<I", PATCH))
    print("hunkCV applied: CV+0x18fd8 mov->cbz (NULL-dict skips CF apply)")


main()
