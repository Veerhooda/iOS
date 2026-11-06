#!/usr/bin/env python3
"""Extract a single image from a dyld shared cache (v1) into a standalone Mach-O.

Usage:
  dyld_extract.py CACHE list [<substr>]
  dyld_extract.py CACHE extract <substr> <outpath>
  dyld_extract.py CACHE attr <hexaddr> [<hexaddr>...]   # image+offset attribution
"""
import struct, sys

def parse_header(d):
    assert d[:7] == b'dyld_v1', d[:16]
    mapOff, mapCount, imgOff, imgCount = struct.unpack('<IIII', d[16:32])
    maps = []
    for i in range(mapCount):
        o = mapOff + i * 32
        addr, size, foff, maxP, initP = struct.unpack('<QQQII', d[o:o+32])
        maps.append((addr, size, foff))
    imgs = []
    for i in range(imgCount):
        o = imgOff + i * 32
        addr, mt, ino, po, pad = struct.unpack('<QQQII', d[o:o+32])
        path = d[po:po+256].split(b'\0')[0].decode()
        imgs.append((addr, path))
    return maps, imgs

def cache_foff(maps, addr):
    for (a, s, f) in maps:
        if a <= addr < a + s:
            return f + (addr - a)
    raise ValueError('addr %s not in any mapping' % hex(addr))

LC_SEGMENT_64 = 0x19

def extract(d, maps, addr):
    """Return standalone Mach-O bytes for image at unslid cache address."""
    hdr_foff = cache_foff(maps, addr)
    magic, cputype, cpusub, ftype, ncmds, szcmds, flags, rsv = struct.unpack(
        '<IIIIIIII', d[hdr_foff:hdr_foff+32])
    assert magic == 0xfeedfacf, hex(magic)
    hdr = bytearray(d[hdr_foff:hdr_foff+32+szcmds])
    # collect segments
    segs = []
    o = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack('<II', hdr[o:o+8])
        if cmd == LC_SEGMENT_64:
            segname = bytes(hdr[o+8:o+24]).split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack('<QQQQ', hdr[o+24:o+56])
            segs.append((o, segname, vmaddr, vmsize, fileoff, filesize))
        o += cmdsize
    out = bytearray(hdr)
    # append segment data contiguously, patch fileoff
    # NOTE: in-cache seg.fileoff is stale; source bytes via mapping(vmaddr)
    for (lo, name, vmaddr, vmsize, fileoff, filesize) in segs:
        if name == '__LINKEDIT':
            # shared cache LINKEDIT is unusable standalone; drop it
            struct.pack_into('<Q', out, lo+40, 0)
            struct.pack_into('<Q', out, lo+48, 0)
            continue
        if filesize == 0:
            continue
        srcoff = cache_foff(maps, vmaddr)
        # clamp to mapping end (shared LINKEDIT over-reports per image)
        for (a, s, f) in maps:
            if a <= vmaddr < a + s:
                filesize = min(filesize, a + s - vmaddr)
        newoff = len(out)
        struct.pack_into('<Q', out, lo+40, newoff)
        struct.pack_into('<Q', out, lo+48, filesize)
        segbytes = bytearray(d[srcoff:srcoff+filesize])
        # v1 cache DATA pointers carry high bits (41/42); strip in pointer-only
        # sections (cache vmaddrs all fit in 33 bits: 0x180000000-0x1c4200000)
        PTR_SECTS = ('__objc_classlist', '__objc_catlist', '__objc_protolist',
                     '__objc_imageinfo', '__objc_const', '__objc_selrefs',
                     '__objc_classrefs', '__objc_superrefs', '__objc_protorefs',
                     '__objc_ivar', '__objc_data', '__got', '__la_symbol_ptr',
                     '__cfstring', '__objc_data', '__objc_methtype')
        if name.startswith('__DATA'):
            nsects0 = struct.unpack('<I', hdr[lo+64:lo+68])[0]
            for s0 in range(nsects0):
                so0 = lo + 72 + s0 * 80
                sn0 = bytes(hdr[so0:so0+16]).split(b'\0')[0].decode()
                if sn0 not in PTR_SECTS:
                    continue
                sa0 = struct.unpack('<Q', hdr[so0+32:so0+40])[0]
                ss0 = struct.unpack('<Q', hdr[so0+40:so0+48])[0]
                base = sa0 - vmaddr
                for qi in range(base, min(base + ss0, len(segbytes)) - 7, 8):
                    q = struct.unpack('<Q', segbytes[qi:qi+8])[0]
                    if q >> 33:
                        c = q & 0x1ffffffff
                        if any(a <= c < a + s for (a, s, f) in maps):
                            struct.pack_into('<Q', segbytes, qi, c)
        # patch section offsets: new = newoff + (sect.addr - seg.vmaddr)
        nsects = struct.unpack('<I', out[lo+64:lo+68])[0]
        for s in range(nsects):
            so = lo + 72 + s * 80
            saddr = struct.unpack('<Q', out[so+32:so+40])[0]
            # section_64: sectname16+segname16+addr8+size8+offset4 -> offset at +48
            struct.pack_into('<I', out, so+48, newoff + (saddr - vmaddr))
        out += segbytes
    # zero out LINKEDIT-based load commands (symtab, dysymtab, dyldinfo, function starts, data-in-code, twolevel hints)
    o = 32
    import struct as st
    ncmds = st.unpack('<I', out[16:20])[0]
    for _ in range(ncmds):
        cmd, cmdsize = st.unpack('<II', out[o:o+8])
        if cmd == 0x2:      # LC_SYMTAB
            st.pack_into('<IIII', out, o+8, 0, 0, 0, 0)
        elif cmd == 0xb:    # LC_DYSYMTAB (zero 18 fields)
            for i in range(18):
                st.pack_into('<I', out, o+8+i*4, 0)
        elif cmd in (0x22, 0x80000022):  # LC_DYLD_INFO[ONLY]
            for i in range(10):
                st.pack_into('<I', out, o+8+i*4, 0)
        elif cmd == 0x26:   # LC_FUNCTION_STARTS
            st.pack_into('<II', out, o+8, 0, 0)
        elif cmd == 0x29:   # LC_DATA_IN_CODE
            st.pack_into('<II', out, o+8, 0, 0)
        elif cmd == 0x1d:   # LC_CODE_SIGNATURE
            st.pack_into('<II', out, o+8, 0, 0)
        o += cmdsize
    return bytes(out)

def main():
    cache, op = sys.argv[1], sys.argv[2]
    d = open(cache, 'rb').read()
    maps, imgs = parse_header(d)
    if op == 'list':
        sub = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        for (a, p) in imgs:
            if sub in p.lower():
                print(hex(a), p)
    elif op == 'extract':
        sub, outp = sys.argv[3], sys.argv[4]
        cands = [(a, p) for (a, p) in imgs if sub.lower() in p.lower()]
        assert len(cands) == 1, cands
        a, p = cands[0]
        print('extracting', hex(a), p, file=sys.stderr)
        open(outp, 'wb').write(extract(d, maps, a))
    elif op == 'attr':
        addrs = sorted(imgs)
        for x in sys.argv[3:]:
            v = int(x, 16)
            best = None
            for (a, p) in addrs:
                if a <= v:
                    best = (a, p)
                else:
                    break
            print(hex(v), '->', best[1].split('/')[-1], '+', hex(v-best[0]))

if __name__ == '__main__':
    main()
