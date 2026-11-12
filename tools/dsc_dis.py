#!/usr/bin/env python3
# Disassemble a range of the iOS dyld shared cache, resolving selector refs and C strings (2026-11-12).
#   python3 tools/dsc_dis.py <cache> <start_va_hex> <end_va_hex>   (needs tools/disas_cache.py; run from repo root)
# dis.py CACHE START_VA END_VA : disassemble a cache range, resolving adrp+ldr x1 selrefs and adrp+add strings
import struct,sys,subprocess,re,os
C=sys.argv[1]; a=int(sys.argv[2],16); b=int(sys.argv[3],16)
f=open(C,'rb'); d=f.read(0x200)
mo,mc=struct.unpack('<II',d[16:24]); f.seek(mo)
maps=[struct.unpack('<QQQII',f.read(32)) for _ in range(mc)]
def rd(va,n):
    for m in maps:
        if m[0]<=va<m[0]+m[1]: f.seek(m[2]+va-m[0]); return f.read(n)
def cstr(va):
    x=rd(va,200); return x.split(b'\0')[0].decode('latin1') if x else '?'
code=rd(a,b-a)
sys.path.insert(0,'tools'); import disas_cache
mach=disas_cache.make_macho(code,a)
p='/tmp/_dis_%d.o'%os.getpid(); open(p,'wb').write(mach)
out=subprocess.run(['objdump','-d','--no-show-raw-insn',p],capture_output=True,text=True).stdout
os.unlink(p)
page={}
for line in out.splitlines():
    m=re.match(r'\s*([0-9a-f]+):\s+(\w+)\s+(.*)',line)
    if not m: continue
    addr,op,args=m.groups(); note=''
    mm=re.match(r'(x\d+), 0x([0-9a-f]+)',args)
    if op=='adrp' and mm: page[mm.group(1)]=int(mm.group(2),16)
    mm=re.match(r'(x\d+), \[(x\d+), #0x([0-9a-f]+)\]',args)
    if op=='ldr' and mm and mm.group(2) in page:
        ptr=struct.unpack('<Q',rd(page[mm.group(2)]+int(mm.group(3),16),8))[0]&0xfffffffff
        s=cstr(ptr); note='  ; '+s if s and s.isprintable() and len(s)>2 else ''
    mm=re.match(r'(x\d+), (x\d+), #0x([0-9a-f]+)',args)
    if op=='add' and mm and mm.group(2) in page:
        s=cstr(page[mm.group(2)]+int(mm.group(3),16)); note='  ; "'+s[:60]+'"' if s and s.isprintable() and len(s)>2 else ''
    print(addr,op,args,note)
