#!/usr/bin/env python3
"""Find ObjC call sites for a selector within a cache image's __TEXT.

Locates the selector cstring, its selref slots in the image __DATA, then
scans image __TEXT for adrp/ldr/add references to those slots followed by
a msgSend-class branch. Reports call-site VMAs.

Usage: op2calls.py CACHE <img_substr> <selector>
"""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from dyld_extract import parse_header, cache_foff

M = 0x1ffffffff

def main():
    cache, substr, sel = sys.argv[1], sys.argv[2], sys.argv[3].encode()
    d = open(cache, 'rb').read()
    maps, imgs = parse_header(d)
    cands = [(a, p) for (a, p) in imgs if substr.lower() in p.lower()]
    assert len(cands) == 1, cands
    base = cands[0][0]
    # 1. selector cstring (first occurrence in TEXT mapping)
    t0, tsz, tfo = maps[0]
    seloff = d[tfo:tfo+tsz].find(sel + b'\0')
    assert seloff > 0, 'selector not in TEXT'
    selva = t0 + seloff
    print('# sel', sel.decode(), 'at', hex(selva), file=sys.stderr)
    # 2. image DATA segments -> selref slots
    foff = cache_foff(maps, base)
    ncmds = struct.unpack('<I', d[foff+16:foff+20])[0]
    o = foff + 32
    datas = []
    textva = textsz = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack('<II', d[o:o+8])
        if cmd == 0x19:
            name = bytes(d[o+8:o+24]).split(b'\0')[0].decode()
            vmaddr, vmsize = struct.unpack('<QQ', d[o+24:o+40])
            nsects = struct.unpack('<I', d[o+64:o+68])[0]
            for s in range(nsects):
                so = o + 72 + s * 80
                sn = bytes(d[so:so+16]).split(b'\0')[0].decode()
                sa, ss = struct.unpack('<QQ', d[so+32:so+48])
                if name == '__TEXT' and sn == '__text':
                    textva, textsz = sa, ss
                if name.startswith('__DATA'):
                    datas.append((sa, ss))
        o += cmdsize
    slots = set()
    for (sa, ss) in datas:
        oo = cache_foff(maps, sa)
        for qi in range(0, ss - 7, 8):
            if struct.unpack('<Q', d[oo+qi:oo+qi+8])[0] & M == selva:
                slots.add(sa + qi)
    print('# %d selref slots' % len(slots), file=sys.stderr)
    # 3. scan __text for adrp->slot refs; check next branch targets msgSend-ish stub
    to = cache_foff(maps, textva)
    code = d[to:to+textsz]
    # collect this image's stub range (for msgSend call check, loose: any bl)
    for i in range(0, len(code) - 16, 4):
        w = struct.unpack('<I', code[i:i+4])[0]
        # adrp xd, page
        if (w & 0x9f000000) != 0x90000000:
            continue
        rd = w & 0x1f
        immhi = (w >> 5) & 0x7ffff
        immlo = (w >> 29) & 0x3
        imm = (immhi << 2) | immlo
        if imm & (1 << 20):
            imm -= 1 << 21
        pc = textva + i
        page = (pc & ~0xfff) + imm * 4096
        # next: ldr xt, [xd, #off] or add xt, xd, #off
        w2 = struct.unpack('<I', code[i+4:i+8])[0]
        tgt = None
        if (w2 & 0xffc00000) == 0xf9400000 and ((w2 >> 5) & 0x1f) == rd:
            tgt = page + ((w2 >> 10) & 0xfff) * 8
        elif (w2 & 0xff000000) == 0x91000000 and ((w2 >> 5) & 0x1f) == rd:
            tgt = page + ((w2 >> 10) & 0xfff)
        if tgt in slots or tgt == selva:
            # look ahead up to 6 insns for bl
            for j in range(2, 8):
                w3 = struct.unpack('<I', code[i+j*4:i+j*4+4])[0]
                if (w3 & 0xfc000000) == 0x94000000:
                    print(hex(pc), 'x%d sel -> bl %s' % (
                        rd, hex(pc + j * 4 + ((((w3 & 0x3ffffff) ^ 0x2000000) - 0x2000000) << 2))))
                    break

if __name__ == '__main__':
    main()
