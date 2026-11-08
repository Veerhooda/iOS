"""MobileAsset fail-fast (hunkMA): skip hung sync asset query in AXUtils.

Hang (run170/171/174, SB main, 3 runs): SB -> AccessibilityUtilities ->
[MAAssetQuery runQueryAndReturnError:] -> MobileAsset sync XPC, which never
returns (message never reaches the healthy-idle mobileassetd; launchd holds
it; SB's sync XPC has no timeout). SB waits forever; server never leaves
BootUI (surface 2, ~24 presents/min).

Site (AccessibilityUtilities, static VA; __TEXT file-first mapping):
  0x190b86030: mov x0, x20          ; x0 = MAAssetQuery object
  0x190b86034: bl  objc_msgSend     ; [query runQueryAndReturnError:&err]
Out-param [sp,#0x28] pre-zeroed; post-call retains the (nil) result and
takes the existing cbz-guarded graceful path (pure ObjC nil-messaging).
Skipping = "query ran, found nothing", which is exactly true on this port
(no assets ever; no network; daemon can't answer).

Patch (2 slots, no trampoline):
  0x190b86030: mov x0,x20 (0xaa1403e0) -> mov x0,xzr (0xaa1f03e0)
  0x190b86034: bl (0x97c3b11f)         -> nop        (0xd503201f)

Usage: ./hunkMA.py <dyld_shared_cache_arm64>   (patches in place; back it up!)
"""
import struct
import sys

VA0 = 0x190B86030
OFF0 = VA0 - 0x180000000
EXPECT0 = 0xAA1403E0  # mov x0, x20
PATCH0 = 0xAA1F03E0   # mov x0, xzr (nil receiver -> nil result)
EXPECT1 = 0x97C3B11F  # bl objc_msgSend
PATCH1 = 0xD503201F   # nop


def main():
    p = sys.argv[1]
    with open(p, "r+b") as f:
        f.seek(OFF0)
        w0 = struct.unpack("<I", f.read(4))[0]
        w1 = struct.unpack("<I", f.read(4))[0]
        if w0 == PATCH0 and w1 == PATCH1:
            print("hunkMA already applied")
            return
        assert w0 == EXPECT0, "site0 mismatch: %s" % hex(w0)
        assert w1 == EXPECT1, "site1 mismatch: %s" % hex(w1)
        f.seek(OFF0)
        f.write(struct.pack("<II", PATCH0, PATCH1))
    print("hunkMA applied: AX+0x6030 query skipped (nil result, graceful)")


main()
