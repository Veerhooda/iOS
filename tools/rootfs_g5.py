#!/usr/bin/env python3
"""G5: room on a mounted iOS rootfs for the GPU Metal driver: rootfs_g5.py <mountpoint>

tools/op2hfs.py writes files on the phone IN PLACE and cannot grow them, so every file the G5 work iterates on
gets a generous zero-padded slot here, once, and afterwards is replaced over adb in seconds:
   /usr/local/lib/op2mtl.dylib                         24 MiB of zeros: the driver library (Mesa fd4 + AIR front end
                                                       behind a C API), loaded by OP2Metal with dlopen; all zeros =
                                                       not installed, OP2Metal keeps its stubs
   /System/Library/Extensions/OP2Metal.bundle/OP2Metal the current build (userland/OP2Metal/build), zero-padded to
                                                       1 MiB (it was a 71552-byte file: no room to grow)
Zero padding after a Mach-O is harmless to dyld (cs_enforcement_disable=1 on our cmdline).
"""
import os
import sys

MiB = 1 << 20
root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))


def slot(path, size, src=None):
    data = open(src, 'rb').read() if src else b''
    if len(data) > size:
        sys.exit('%s is %d bytes; its slot is %d' % (src, len(data), size))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
        f.write(b'\0' * (size - len(data)))
    os.chown(path, 0, 0)
    os.chmod(path, 0o755)
    print('  [G5] %s: %d-byte slot%s' % (path[len(root):], size, ' (%d bytes of %s)' % (len(data), src) if src else ''))


slot(os.path.join(root, 'usr/local/lib/op2mtl.dylib'), 24 * MiB)
op2metal = os.path.join(here, '..', 'userland', 'OP2Metal', 'build', 'OP2Metal.bundle', 'OP2Metal')
if os.path.exists(op2metal):
    slot(os.path.join(root, 'System/Library/Extensions/OP2Metal.bundle/OP2Metal'), MiB, op2metal)
