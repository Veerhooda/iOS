#!/usr/bin/env python3
"""Dump all ObjC methods (IMP -> class selector) for a cache image, or match PCs.

Usage:
  op2methods.py CACHE <img_substr> [--match 0xIMP,0xIMP...]
Reads ObjC metadata straight from cache bytes (masks bit41/42 high bits).
"""
import struct, sys, bisect
sys.path.insert(0, __import__('os').path.dirname(__file__))
from dyld_extract import parse_header, cache_foff

M = 0x1ffffffff

def methods_for_image(d, maps, imgs, substr):
    cands = [(a, p) for (a, p) in imgs if substr.lower() in p.lower()]
    assert len(cands) == 1, cands
    base, path = cands[0]
    # read image load commands to find __objc_classlist/__objc_catlist
    foff = cache_foff(maps, base)
    ncmds = struct.unpack('<I', d[foff+16:foff+20])[0]
    o = foff + 32
    classlist = catlist = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack('<II', d[o:o+8])
        if cmd == 0x19:
            nsects = struct.unpack('<I', d[o+64:o+68])[0]
            for s in range(nsects):
                so = o + 72 + s * 80
                sn = bytes(d[so:so+16]).split(b'\0')[0].decode()
                if sn in ('__objc_classlist', '__objc_catlist'):
                    sa, ss = struct.unpack('<QQ', d[so+32:so+48])
                    if sn == '__objc_classlist':
                        classlist = (sa, ss)
                    else:
                        catlist = (sa, ss)
        o += cmdsize
    out = []

    def cstr(v):
        v &= M
        oo = cache_foff(maps, v)
        return d[oo:d.index(b'\0', oo)].decode(errors='replace')

    def mlist(ml, cls, meta):
        ml &= M
        try:
            oo = cache_foff(maps, ml)
        except ValueError:
            return
        entsz, cnt = struct.unpack('<II', d[oo:oo+8])
        st = entsz & ~3
        if st not in (12, 24):
            return
        for i in range(cnt):
            e = oo + 8 + i * st
            if st == 24:
                nm, ty, imp = struct.unpack('<QQQ', d[e:e+24])
                out.append((imp & M, '%s%s %s' % ('+' if meta else '-', cls, cstr(nm))))
            else:
                no, to, impo = struct.unpack('<iii', d[e:e+12])
                out.append(((e + impo) and M or 0, '%s%s %s' % ('+' if meta else '-', cls, 'rel')))

    def klass(k, meta_ok=True):
        k &= M
        try:
            oo = cache_foff(maps, k)
        except ValueError:
            return
        isa, sup, cache, vtbl, data = struct.unpack('<QQQQQ', d[oo:oo+40])
        dro = (data & ~0x7) & M
        try:
            ro = cache_foff(maps, dro)
        except ValueError:
            return
        f, iS, iZ, r, iv, name, bM, bP, ivs, wiv, prop = struct.unpack(
            '<IIIIQQQQQQQ', d[ro:ro+72])
        cls = cstr(name)
        mlist(bM, cls, not meta_ok)
        if meta_ok and isa:
            klass(isa, False)

    if classlist:
        sa, ss = classlist
        oo = cache_foff(maps, sa)
        for i in range(ss // 8):
            k = struct.unpack('<Q', d[oo+i*8:oo+i*8+8])[0]
            klass(k)
    if catlist:
        sa, ss = catlist
        oo = cache_foff(maps, sa)
        for i in range(ss // 8):
            k = struct.unpack('<Q', d[oo+i*8:oo+i*8+8])[0] & M
            try:
                co = cache_foff(maps, k)
            except ValueError:
                continue
            nm, cls, im, cm, pr, prop = struct.unpack('<QQQQQQ', d[co:co+48])
            try:
                cn = cstr(cls)
            except ValueError:
                cn = 'cat?'
            mlist(im, cn + '(cat)', False)
            mlist(cm, cn + '(cat)', True)
    out.sort()
    return base, path, out

def main():
    cache, substr = sys.argv[1], sys.argv[2]
    match = []
    if '--match' in sys.argv:
        mi = sys.argv.index('--match')
        match = [int(x, 16) for x in sys.argv[mi+1].split(',')]
    d = open(cache, 'rb').read()
    maps, imgs = parse_header(d)
    base, path, meths = methods_for_image(d, maps, imgs, substr)
    print('#', hex(base), path, len(meths), 'methods', file=sys.stderr)
    if match:
        imps = [m[0] for m in meths]
        for t in match:
            i = bisect.bisect_right(imps, t) - 1
            print(hex(t), '=', meths[i][1], '+', hex(t - meths[i][0]))
    else:
        for (imp, name) in meths:
            print(hex(imp), name)

if __name__ == '__main__':
    main()
