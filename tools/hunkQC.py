#!/usr/bin/env python3
"""QuartzCore commit NULL-guard (hunkQC): trampoline patch for the
backboardd/SpringBoard `x0=NULL` crash at QC commit+0x14 (VA 0x1852f7648).

Crash: helper (0x1852f7680) returns NULL when Metal is missing ->
commit's `ldr x0,[x0]` faults (SEGV x0=0). 1-slot cbz can't work (it would
delete the load the normal path needs), so:

  P1 @ 0x1852f7648: `ldr x0,[x0]` -> `bl tramp`      (4 bytes)
  TRAMP @ 0x185381958 (verified 12B nop padding: ret|nop,nop,nop|code):
      cbz x0, <commit epilogue 0x1852f7674>   ; NULL -> balanced exit
      ldr x0, [x0]                            ; else original load
      ret                                     ; -> commit+0x18 (lr set by bl)

NULL path: commit returns NULL -> commit#2's existing `cbz x0` takes its
graceful release path (frame skipped, no crash). Normal path: bit-identical.
lr clobber is safe (commit restores x30 from its own stack save).

Usage: ./hunkQC.py <dyld_shared_cache_arm64>   (patches in place; back it up!)
"""
import struct, sys

P1_VA, TRAMP_VA, EPIL_VA = 0x1852F7648, 0x185381958, 0x1852F7674

def enc_bl(frm, to):
    d = (to - frm) // 4
    assert -(1 << 25) <= d < (1 << 25), 'bl out of range'
    return struct.pack('<I', 0x94000000 | (d & 0x3FFFFFF))

def enc_cbz_x0(frm, to):
    d = (to - frm) // 4
    assert -(1 << 18) <= d < (1 << 18), 'cbz out of range'
    return struct.pack('<I', 0xB4000000 | ((d & 0x7FFFF) << 5))

def main():
    p = sys.argv[1]
    d = bytearray(open(p, 'rb').read())
    p1 = P1_VA - 0x180000000
    tr = TRAMP_VA - 0x180000000
    assert d[p1:p1+4] == struct.pack('<I', 0xF9400000), 'P1 site changed!'
    assert d[tr:tr+12] == struct.pack('<I', 0xD503201F) * 3, 'pad changed!'
    assert d[tr-4:tr] == struct.pack('<I', 0xD65F03C0), 'no ret before pad!'
    d[p1:p1+4] = enc_bl(P1_VA, TRAMP_VA)
    d[tr:tr+12] = (enc_cbz_x0(TRAMP_VA, EPIL_VA)
                   + struct.pack('<I', 0xF9400000)
                   + struct.pack('<I', 0xD65F03C0))
    open(p, 'r+b').write(d)
    print('hunkQC applied: P1=bl->%x tramp=[cbz->%x,ldr,ret]'
          % (TRAMP_VA, EPIL_VA))

if __name__ == '__main__':
    main()
