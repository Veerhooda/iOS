#!/usr/bin/env python3
"""Verify dyld-cache slide for an OP2 run log via dual anchors.

Anchors (ground truth from cache bytes):
  SVC (pthread_kill insn+4) = 0x180a630d8
  libdyld _start+4          = 0x1809168e0
ESTK slots hold return addresses (call+4), hence the +4s.
Usage: op2slide.py logs/run105_ramoops.txt
"""
import re, sys

SVC4 = 0x180a630dc  # pthread_kill: thread reaped at svc+4 (EINTR return)
START4 = 0x1809168e0  # libdyld _start+4

def slides_in_log(path):
    txt = open(path, errors='replace').read()
    # trap pcs from sig=6 EXIT lines only (pthread_kill svc+4 pattern)
    traps = [int(m.group(1), 16)
             for m in re.finditer(r'sig=6 code=\d+ pc=(0x[0-9a-f]+) lr=', txt)
             if int(m.group(1), 16) > 0x180000000]
    # candidate libdyld frames: ESTK slots whose low12 == 0x8e0 (start+4 fingerprint)
    estk = []
    for m in re.finditer(r'OP2 ESTK #\d+(?:\+\d+)?: ([0-9a-fx,]+)', txt):
        estk += [int(x, 16) for x in m.group(1).split(',') if x.startswith('0x')]
    cands = sorted(set(v for v in estk if v and (v & 0xfff) == 0x8e0))
    return traps, cands

def main():
    path = sys.argv[1]
    traps, cands = slides_in_log(path)
    from collections import Counter
    tvotes = Counter()
    for t in traps:
        s = t - SVC4
        if s & 0x3fff == 0:
            tvotes[s] += 1
    print(path, 'trap-pcs:', len(traps), 'aligned-slide votes:', dict(tvotes))
    svotes = Counter()
    for c in cands:
        s = c - START4
        if s & 0x3fff == 0:
            svotes[s] += 1
    print(path, 'start4-cands:', len(cands), 'aligned-slide votes:', dict(svotes))
    both = set(tvotes) & set(svotes)
    if len(both) == 1:
        print(path, 'SLIDE =', hex(both.pop()))
    else:
        print(path, 'NO CONSENSUS:', [hex(x) for x in both])

if __name__ == '__main__':
    main()
