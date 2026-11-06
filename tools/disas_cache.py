import os, sys, struct, subprocess

CACHE_PATH = os.environ.get("OP2_DYLD_CACHE",
    "/tmp/fixaro/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64")

def make_macho(code, va):
    magic = 0xfeedfacf
    cputype = 0x0100000c
    cpusubtype = 0
    filetype = 1
    ncmds = 1
    sizeofcmds = 72 + 80
    flags = 0
    reserved = 0
    hdr = struct.pack("<IIIIIIII", magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved)
    cmd = 0x19
    cmdsize = 72 + 80
    segname = b"__TEXT\0".ljust(16, b"\0")
    vmaddr = va
    vmsize = len(code)
    fileoff = 32 + cmdsize
    filesize = len(code)
    maxprot = 7
    initprot = 5
    nsects = 1
    seg_flags = 0
    seg = struct.pack("<II16sQQQQiiII", cmd, cmdsize, segname, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, seg_flags)
    sectname = b"__text\0".ljust(16, b"\0")
    segname2 = b"__TEXT\0".ljust(16, b"\0")
    sect = struct.pack("<16s16sQQIIIIIIII", sectname, segname2, va, len(code), fileoff, 2, 0, 0, 0x80000400, 0, 0, 0)
    return hdr + seg + sect + code

def disas(va, size):
    file_off = va - 0x180000000
    with open(CACHE_PATH, "rb") as f:
        f.seek(file_off)
        data = f.read(size)
    macho = make_macho(data, va)
    with open("/tmp/disas.o", "wb") as f:
        f.write(macho)
    res = subprocess.run(["otool", "-tV", "/tmp/disas.o"], capture_output=True, text=True)
    lines = res.stdout.splitlines()
    if len(lines) > 2:
        print("\n".join(lines[2:]))
    else:
        print(res.stdout)

if __name__ == "__main__":
    va = int(sys.argv[1], 16) if sys.argv[1].startswith("0x") else int(sys.argv[1])
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 128
    disas(va, size)
