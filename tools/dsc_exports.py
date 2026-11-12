#!/usr/bin/env python3
# Exported symbols of one shared-cache image: python3 tools/dsc_exports.py <cache> <image_base_hex> <export_off> <export_size> [names...]
# Get export_off/size with tools/dsc_export_off.py <cache> <image_base_hex>; image bases from tools/dyld_extract.py <cache> list <name>
import struct,sys
# usage: exports.py CACHE IMAGE_BASE_VA export_off export_size [names...]
C=sys.argv[1]; base=int(sys.argv[2],16); off=int(sys.argv[3]); size=int(sys.argv[4]); want=set(sys.argv[5:])
f=open(C,'rb'); f.seek(off); t=f.read(size)
def uleb(p):
    r=s=0
    while True:
        b=t[p]; p+=1; r|=(b&0x7f)<<s; s+=7
        if b<0x80: return r,p
out={}
def walk(p,pref):
    ts,q=uleb(p)
    if ts:
        fl,q2=uleb(q); addr,_=uleb(q2)
        if pref in want or not want: out[pref]=base+addr
    q+=ts; n=t[q]; q+=1
    for _ in range(n):
        e=t.index(b'\0',q); s=t[q:e].decode(); q=e+1
        c,q=uleb(q); walk(c,pref+s)
walk(0,'')
for k,v in sorted(out.items(), key=lambda kv: kv[1]): print(hex(v),k)
