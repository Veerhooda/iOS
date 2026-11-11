#!/usr/bin/env python3
"""Add (or replace) launchd jobs in iOS 12's job cache, keeping the file's size and layout.

launchd on iOS 12 loads its jobs from /System/Library/Caches/com.apple.xpcd/xpcd_cache.dylib: a binary plist
in section __TEXT,__xpcd_cache whose LaunchDaemons dict maps each job plist's path to the job. A plist dropped
into /System/Library/LaunchDaemons is not seen unless it is in here too (boot-arg launchd_unsecure_cache=1 lets
launchd take a cache that is no longer Apple's). The new plist is padded to the section's exact size (zeros
between the objects and the offset table, the trailer pointing past them), so the file can be rewritten in
place by tools/op2hfs.py.
   xpcd-cache-add.py IN.dylib OUT.dylib JOB.plist [JOB.plist ...]
"""
import plistlib, struct, sys

def section(data):
    ncmds = struct.unpack_from('<I', data, 16)[0]
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from('<I', data, off + 64)[0]
            s = off + 72
            for _ in range(nsects):
                name = data[s:s + 16].rstrip(b'\0').decode()
                sz = struct.unpack_from('<Q', data, s + 40)[0]
                fo = struct.unpack_from('<I', data, s + 48)[0]
                if name == '__xpcd_cache':
                    return fo, sz
                s += 80
        off += size
    raise SystemExit('no __xpcd_cache section')

def padded(obj, size):
    b = plistlib.dumps(obj, fmt=plistlib.FMT_BINARY, sort_keys=False)
    if len(b) > size:
        raise SystemExit('plist is %d bytes, section holds %d' % (len(b), size))
    trailer = b[-32:]
    table_off = struct.unpack('>Q', trailer[24:32])[0]
    pad = size - len(b)
    out = b[:table_off] + b'\0' * pad + b[table_off:-32] + trailer[:24] + struct.pack('>Q', table_off + pad)
    assert len(out) == size and plistlib.loads(out) == obj
    return out

data = bytearray(open(sys.argv[1], 'rb').read())
fo, sz = section(data)
cache = plistlib.loads(bytes(data[fo:fo + sz]))
for job_path in sys.argv[3:]:
    job = plistlib.load(open(job_path, 'rb'))
    key = '/System/Library/LaunchDaemons/%s.plist' % job['Label']
    cache['LaunchDaemons'][key] = job
    print('job %s -> %s' % (job['Label'], key))
data[fo:fo + sz] = padded(cache, sz)
open(sys.argv[2], 'wb').write(data)
print('section at %d, %d bytes; %d jobs; wrote %s' % (fo, sz, len(cache['LaunchDaemons']), sys.argv[2]))
