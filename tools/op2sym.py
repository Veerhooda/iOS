"""Map iOS user PCs to dylib+file-offset using a dyld shared cache.

Usage: op2sym.py <cache> <slide> <pc>... [--exec <main-exec-base>]
  slide: run-specific cache slide, e.g. 0xb1d50000 (from op2slide.py)
  pc:    runtime user PC (0x2... cache VA or 0x10... main-exec VA)
  --exec: runtime base of the main executable (from SNAP pid line) for
          0x10... PCs; prints file offset (static base assumed 0x100000000).

Example: op2sym.py cache 0xb1d50000 0x232ba8914 0x100b39b98 --exec 0x100950000
"""
import struct
import sys


def load_images(f):
    hdr = f.read(32)
    _, _, img_off, img_cnt = struct.unpack("<IIII", hdr[16:32])
    f.seek(img_off)
    raw = f.read(img_cnt * 32)
    imgs = []
    for i in range(img_cnt):
        addr, _, _, poff, _ = struct.unpack_from("<QQQII", raw, i * 32)
        imgs.append((addr, poff))
    return sorted(imgs)


def main():
    args = sys.argv[1:]
    exec_base = 0
    if "--exec" in args:
        i = args.index("--exec")
        exec_base = int(args[i + 1], 16)
        del args[i:i + 2]
    cache, slide_s, pcs = args[0], args[1], args[2:]
    slide = int(slide_s, 16)
    f = open(cache, "rb")
    imgs = load_images(f)
    addrs = [a for a, _ in imgs]
    import bisect

    for p in pcs:
        pc = int(p, 16)
        if pc < 0x180000000 and exec_base:
            print("%s -> main-exec file off %s" % (p, hex(pc - exec_base)))
            continue
        static = pc - slide
        j = bisect.bisect_right(addrs, static) - 1
        if j < 0:
            print("%s -> static %s (before first image)" % (p, hex(static)))
            continue
        base, poff = imgs[j]
        f.seek(poff)
        path = f.read(300).split(b"\0")[0].decode(errors="replace")
        print("%s -> static %s in %s + %s"
              % (p, hex(static), path.split("/")[-1], hex(static - base)))


main()
