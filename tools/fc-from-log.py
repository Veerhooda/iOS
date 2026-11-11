#!/usr/bin/env python3
"""Function constants CoreAnimation asked Metal for, out of a ramoops log (OP2Metal's capture, metal=2):
   OP2 GPU user: fc <prog> <function> <class>
   OP2 GPU user: fc+ #<index>=t<MTLDataType hex>:<value bytes, little-endian hex> ...
Prints one line per (prog, function): "<prog> <function> AIR_FC=<index>=<value>,..." for tools/ca-sweep.sh."""
import re, sys

cur, out = None, []
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'OP2 GPU user: fc (\S+) (\S+)', line)
    if m:
        cur = [m.group(1), m.group(2), []]
        out.append(cur)
        continue
    m = re.search(r'OP2 GPU user: fc\+(.*)', line)
    if m and cur:
        for idx, t, hexv in re.findall(r'#(\d+)=t([0-9a-f]+):([0-9a-f]*)', m.group(1)):
            b = bytes.fromhex(hexv)
            cur[2].append('%d=0x%x' % (int(idx), int.from_bytes(b, 'little')))
for prog, fn, fc in out:
    print('%s %s AIR_FC=%s' % (prog, fn, ','.join(fc)))
