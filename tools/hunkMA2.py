"""MobileAsset fail-fast at the source (hunkMA2): stub the sync query entries.

Why (run175/loop-28): hunkMA (AX call-site skip) WORKED -- SB sailed past
the AccessibilityUtilities path -- then parked at the NEXT sync MA query,
this time via TextToSpeech -> ??? -> ASAssetQuery. Same MA trap, different
caller. Whack-a-mole at call sites cannot win; the fix belongs in MA.

Hang chain (both run174 AX path and run175 TTS path):
  -[ASAssetQuery runQueryAndReturnError:] +0x14
    -> -[ASAssetQuery runQueryForInfoReturnError:] +0x9c
      -> static helper -> libxpc+0xa850 (sync send, no timeout) -> mach_msg
  The message never reaches the healthy-idle mobileassetd (launchd holds
  it); SB main waits forever; server never leaves BootUI (surface 2).

Fix: stub the 4 sync-query entries to fail fast. Return-type encodings
verified from ObjC metadata (@=object, q=long long):
  AS runQueryAndReturnError:      0x187ae86e8  '@' -> return nil
  AS runQueryForInfoReturnError:  0x187ae8414  '@' -> return nil
  MA queryMetaDataSync            0x187ae5fbc  'q' -> return 0 (calls sync xpc +0x210)
  MA queryInstalledAssetIds       0x187ae5c88  'q' -> return 0 (calls sync xpc +0x100)
"Found nothing" is exactly true on this port (no assets ever; no network;
daemon cannot answer). Callers take their normal empty-results paths
(nil-messaging / zero-count). Download/purge Sync methods intentionally
left alone (not boot-path; minimal scope).

Patch (2 slots each, no trampoline):
  entry+0: <prologue word0> -> mov x0,xzr (0xaa1f03e0)
  entry+4: <prologue word1> -> ret       (0xd65f03c0)

Usage: ./hunkMA2.py <dyld_shared_cache_arm64>   (patches in place; back it up!)
"""
import struct
import sys

PATCH = (0xAA1F03E0, 0xD65F03C0)  # mov x0,xzr ; ret

SITES = [
    # (VA, expect_w0, expect_w1, name)
    (0x187AE86E8, 0xA9BF7BFD, 0x910003FD, 'AS runQueryAndReturnError:'),
    (0x187AE8414, 0xD10603FF, 0xA9126FFC, 'AS runQueryForInfoReturnError:'),
    (0x187AE5FBC, 0xD10183FF, 0xA90167FA, 'MA queryMetaDataSync'),
    (0x187AE5C88, 0xD10183FF, 0xA90167FA, 'MA queryInstalledAssetIds'),
]


def main():
    p = sys.argv[1]
    with open(p, 'r+b') as f:
        for va, e0, e1, name in SITES:
            f.seek(va - 0x180000000)
            w0, w1 = struct.unpack('<II', f.read(8))
            if (w0, w1) == PATCH:
                print('already applied: %s' % name)
                continue
            assert w0 == e0, '%s site0 mismatch: %s' % (name, hex(w0))
            assert w1 == e1, '%s site1 mismatch: %s' % (name, hex(w1))
            f.seek(va - 0x180000000)
            f.write(struct.pack('<II', *PATCH))
            print('patched: %s' % name)
    print('hunkMA2 done: 4 sync-query entries fail fast (nil/0)')


main()
