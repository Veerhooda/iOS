#!/usr/bin/env python3
# Print LC_DYLD_INFO export_off and export_size of a shared-cache image: python3 tools/dsc_export_off.py <cache> <image_base_hex>
import struct,sys
f=open(sys.argv[1],'rb'); base=int(sys.argv[2],16)
off=base-0x180000000; f.seek(off); h=f.read(32); ncmds=struct.unpack('<I',h[16:20])[0]; p=off+32
for i in range(ncmds):
    f.seek(p); cmd,sz=struct.unpack('<II',f.read(8))
    if cmd in (0x80000022,0x22):
        f.seek(p); v=struct.unpack('<12I',f.read(48)); print(v[10],v[11])
    p+=sz
