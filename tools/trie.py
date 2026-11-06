"""Dump the dyld export trie of a Mach-O as addr -> name lines.

Usage: trie.py <macho> [substr]
Only ET_DYLIB / MH_EXECUTE arm64 with LC_DYLD_INFO(_ONLY) are supported.
"""
import struct
import sys


def uleb(d, o):
    r = s = 0
    while True:
        b = d[o]
        o += 1
        r |= (b & 0x7F) << s
        if not b & 0x80:
            return r, o
        s += 7


def main():
    path, sub = sys.argv[1], sys.argv[2].lower() if len(sys.argv) > 2 else ""
    d = open(path, "rb").read()
    assert struct.unpack_from("<I", d, 0)[0] == 0xFEEDFACF, "not arm64 mach-o"
    ncmds = struct.unpack_from("<I", d, 16)[0]
    o, eoff, esize = 32, 0, 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, o)
        if cmd in (0x22, 0x80000022):
            eoff, esize = struct.unpack_from("<II", d, o + 8 + 32)
        o += size
    assert eoff and esize, "no export trie"
    out = []

    def walk(node, prefix):
        tsize, p = uleb(d, eoff + node)
        if tsize:
            _, p = uleb(d, p)  # flags
            addr, p = uleb(d, p)
            out.append((addr, prefix))
        else:
            p = eoff + node + 1
        nc = d[p]
        p += 1
        for _ in range(nc):
            e = d[p:].split(b"\0")[0]
            p += len(e) + 1
            coff, p = uleb(d, p)
            walk(coff, prefix + e.decode("utf-8", "replace"))

    walk(0, "")
    for a, n in sorted(out):
        if sub in n.lower():
            print(hex(a), n)
    if not sub:
        print("(%d exports)" % len(out), file=sys.stderr)


main()
