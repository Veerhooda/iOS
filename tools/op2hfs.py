#!/usr/bin/env python3
"""Minimal HFS+/HFSX reader/writer for the on-device iOS userdata volume.

Reads/writes file bytes on /dev/block/bootdevice/by-name/userdata via adb
without mounting (TWRP lacks hfsplus). Used for surgical dyld-cache hunk
application and ground-truth reads (AT plist, park state).

Usage:
  op2hfs.py ls <dirpath>              # list directory (names + ids)
  op2hfs.py cat <path> [off] [len]    # dump bytes (stdout)
  op2hfs.py read <path> <off> <len> <out>  # read bytes to file
  op2hfs.py write <path> <off> <infile>    # write bytes (DATA ONLY, in-place)
"""
import struct
import subprocess
import sys

DEV = '/dev/block/bootdevice/by-name/userdata'
BS = 4096
CAT_START = 23601  # catalog fork start block (from volume header)
EXT_START = 49     # extents overflow fork start block


def rdd(skip_blocks, count_blocks):
    """Read raw blocks from device; return exact bytes."""
    want = count_blocks * BS
    p = subprocess.run(['adb', 'exec-out',
                        'dd if=%s bs=%d skip=%d count=%d 2>/dev/null'
                        % (DEV, BS, skip_blocks, count_blocks)],
                       capture_output=True)
    if len(p.stdout) < want:
        raise RuntimeError('short read: got %d want %d' % (len(p.stdout), want))
    return p.stdout[:want]


def wrd(skip_blocks, data):
    """Write bytes at block offset (must be block-aligned len via dd)."""
    assert len(data) % BS == 0
    import tempfile
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(data)
        tmp = t.name
    rpath = '/tmp/op2hfs_w.bin'
    try:
        p = subprocess.run(['adb', 'push', tmp, rpath], capture_output=True)
        if p.returncode != 0:
            raise RuntimeError('push failed: %s' % p.stderr.decode(errors='replace'))
        p = subprocess.run(['adb', 'shell',
                            'dd if=%s of=%s bs=%d seek=%d conv=notrunc 2>&1 && sync && rm %s'
                            % (rpath, DEV, BS, skip_blocks, rpath)],
                           capture_output=True)
        out = p.stdout.decode(errors='replace')
        if p.returncode != 0 or 'records out' not in out.replace('\r', ''):
            raise RuntimeError('write failed: %s' % out)
        return out
    finally:
        import os
        os.unlink(tmp)


def u16(b, o):
    return struct.unpack('>H', b[o:o + 2])[0]


def u32(b, o):
    return struct.unpack('>I', b[o:o + 4])[0]


class BTree:
    def __init__(self, start_block):
        hdr = rdd(start_block, 4)  # header node (assume <=16K; resize below)
        self.node_size = u16(hdr, 32)
        self.root = u32(hdr, 16)
        self.first_leaf = u32(hdr, 24)
        self.start = start_block
        nn = self.node_size // BS
        self.blk_per_node = nn if nn >= 1 else 1
        # re-read header fully if node bigger than 16K
        if self.node_size > 4 * BS:
            hdr = rdd(start_block, self.node_size // BS)
        self.hdr = hdr

    def blk(self, fb):
        """Device block of the catalog's file block fb, through the fork's extents (the volume header's eight;
        the catalog grew a second extent by round fable-40, after which the old start+offset read garbage)."""
        if getattr(self, 'extents', None) is None:
            vh = rdd(0, 1)[1024:1536]
            self.extents = parse_extents(vh, 0x110 + 16)
        for s, c in self.extents:
            if fb < c:
                return s + fb
            fb -= c
        raise IOError('catalog block beyond its eight extents (extents overflow file not read)')

    def node(self, n):
        nb = self.node_size // BS
        if nb < 1:
            # node smaller than block: read block, slice
            per = BS // self.node_size
            blk = rdd(self.blk(n // per), 1)
            return blk[(n % per) * self.node_size:(n % per + 1) * self.node_size]
        first = self.blk(n * nb)
        if self.blk(n * nb + nb - 1) == first + nb - 1:
            return rdd(first, nb)
        return b''.join(rdd(self.blk(n * nb + i), 1) for i in range(nb))


def parse_extents(b, o):
    out = []
    for i in range(8):
        s, c = struct.unpack('>II', b[o + i * 8:o + i * 8 + 8])
        if c:
            out.append((s, c))
    return out


def fork_len(b, o):
    return struct.unpack('>Q', b[o:o + 8])[0]


CAT = None


def cat_init():
    global CAT
    if CAT is None:
        CAT = BTree(CAT_START)
    return CAT


def rec_offs(nd):
    nrec = u16(nd, 10)
    L = len(nd)
    return [u16(nd, L - 2 * (i + 1)) for i in range(nrec + 1)]


def find_record(parent_id, name):
    """Walk catalog B-tree; return (record_bytes incl. key) or None."""
    bt = cat_init()
    # key compare: parentID u32 then name bytes
    n = bt.root
    while True:
        nd = bt.node(n)
        kind = nd[8]
        nrec = u16(nd, 10)
        offs = rec_offs(nd)
        if kind == 0xFF:  # leaf
            for i in range(nrec):
                rec = nd[offs[i]:offs[i + 1]]
                klen = u16(rec, 0)
                k = rec[2:2 + klen]
                # k = parentID(4) + namelen(2) + name
                kpid = u32(k, 0)
                kn = k[6:]
                if kpid == parent_id and kn == name.encode('utf-16-be'):
                    return rec[2 + klen:]
            return None
        elif kind == 0x00:  # index
            # descend: last record with key <= target
            child = None
            for i in range(nrec):
                rec = nd[offs[i]:offs[i + 1]]
                klen = u16(rec, 0)
                k = rec[2:2 + klen]
                kpid = u32(k, 0)
                kn = k[6:]
                ck = (kpid, kn)
                if ck <= (parent_id, name.encode('utf-16-be')):
                    child = u32(rec, 2 + klen)
                else:
                    break
            if child is None:
                # take first
                rec = nd[offs[0]:offs[1]]
                klen = u16(rec, 0)
                child = u32(rec, 2 + klen)
            n = child
        else:
            raise RuntimeError('unexpected node kind %d' % kind)


def resolve(path):
    """Resolve absolute path -> (fileID, is_dir, data_extents, logical_size)."""
    parts = [p for p in path.split('/') if p]
    pid = 2  # root folder cnid
    rec = None
    for i, part in enumerate(parts):
        rec = find_record(pid, part)
        if rec is None:
            raise RuntimeError('not found: %s (at %s)' % (path, part))
        rtype = u16(rec, 0)
        last = (i == len(parts) - 1)
        if rtype == 0x0001:  # folder
            pid = u32(rec, 8)  # folderID
            if last:
                return (pid, True, [], 0)
        elif rtype == 0x0002:  # file
            fid = u32(rec, 8)
            if not last:
                raise RuntimeError('not a dir: %s' % part)
            size = fork_len(rec, 88)
            exts = parse_extents(rec, 88 + 16)
            return (fid, False, exts, size)
        else:
            raise RuntimeError('bad record type %d at %s' % (rtype, part))
    raise RuntimeError('empty path')


def file_blocks(exts, size):
    """Expand data extents to block list (contiguous runs)."""
    out = []
    for s, c in exts:
        out.extend(range(s, s + c))
    return out


def read_file(path, off, length):
    fid, isdir, exts, size = resolve(path)
    if isdir:
        raise RuntimeError('is a directory: %s' % path)
    if off + length > size:
        raise RuntimeError('read past EOF (%d > %d)' % (off + length, size))
    blocks = file_blocks(exts, size)
    start_blk = off // BS
    end_blk = (off + length + BS - 1) // BS
    # coalesce run reads
    out = b''
    run = []
    blks = blocks[start_blk:end_blk]
    for b in blks:
        run.append(b)
    # read each (simple; runs could merge but counts are small)
    # merge consecutive
    i = 0
    data = b''
    while i < len(blks):
        j = i
        while j + 1 < len(blks) and blks[j + 1] == blks[j] + 1:
            j += 1
        data += rdd(blks[i], j - i + 1)
        i = j + 1
    start_off = off - start_blk * BS
    return data[start_off:start_off + length]


def write_file(path, off, payload):
    fid, isdir, exts, size = resolve(path)
    if isdir:
        raise RuntimeError('is a directory')
    if off + len(payload) > size:
        raise RuntimeError('write past EOF')
    blocks = file_blocks(exts, size)
    # read-modify-write affected blocks
    first = off // BS
    last = (off + len(payload) - 1) // BS
    for bi in range(first, last + 1):
        blk = rdd(blocks[bi], 1)
        a = max(off, bi * BS) - bi * BS
        z = min(off + len(payload), (bi + 1) * BS) - bi * BS
        blk = blk[:a] + payload[bi * BS + a - off:bi * BS + z - off] + blk[z:]
        wrd(blocks[bi], blk)


def ls_dir(path):
    """List directory via thread records scan (slow) -- instead scan leaf chain
    for our parent cnid. Simple approach: walk all leaves from leftmost."""
    parts = [p for p in path.split('/') if p]
    pid = 2
    for part in parts:
        rec = find_record(pid, part)
        if rec is None or u16(rec, 0) != 0x0001:
            raise RuntimeError('no such dir: %s' % path)
        pid = u32(rec, 8)
    bt = cat_init()
    n = bt.first_leaf
    out = []
    while n != 0:
        nd = bt.node(n)
        nrec = u16(nd, 10)
        offs = rec_offs(nd)
        for i in range(nrec):
            rec = nd[offs[i]:offs[i + 1]]
            klen = u16(rec, 0)
            k = rec[2:2 + klen]
            if u32(k, 0) == pid:
                nl = u16(k, 4)
                nm = k[6:6 + nl * 2].decode('utf-16-be')
                body = rec[2 + klen:]
                rt = u16(body, 0)
                if rt in (1, 2):
                    out.append(('d' if rt == 1 else 'f', nm))
        n = u32(nd, 0)  # fLink
    return out


def main():
    a = sys.argv[1]
    if a == 'ls':
        for t, n in ls_dir(sys.argv[2]):
            print(t, n)
    elif a == 'cat':
        p = sys.argv[2]
        off = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
        ln = int(sys.argv[4], 0) if len(sys.argv) > 4 else 64
        sys.stdout.buffer.write(read_file(p, off, ln))
    elif a == 'read':
        p, off, ln, out = sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0), sys.argv[5]
        open(out, 'wb').write(read_file(p, off, ln))
        print('wrote %s' % out)
    elif a == 'write':
        p, off, inf = sys.argv[2], int(sys.argv[3], 0), sys.argv[4]
        write_file(p, off, open(inf, 'rb').read())
        print('wrote %d bytes at %s+%s' % (len(open(inf, 'rb').read()), p, sys.argv[3]))
    else:
        sys.exit('unknown cmd')


if __name__ == '__main__':
    main()
