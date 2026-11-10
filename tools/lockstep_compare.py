#!/usr/bin/env python3
# lockstep_compare.py <logA> <logB>: the first game frame two RDR_LOCKSTEP_HASH=1 runs part (docs/ONLINE.md)
import sys, re
def load(p):
    d={}
    for l in open(p, errors='replace'):
        m=re.match(r'\[LS\] frame (\d+) hash (\w+) p0 (\S+) (\S+) (\S+) rng (\w+) (\w+)', l)
        if m: d[int(m.group(1))]=m.groups()[1:]
    return d
a,b=load(sys.argv[1]),load(sys.argv[2])
common=sorted(set(a)&set(b))
print('frames A',len(a),'B',len(b),'common',len(common), 'A max',max(a) if a else 0,'B max',max(b) if b else 0)
bad=[f for f in common if a[f][0]!=b[f][0]]
print('differing frames',len(bad))
if bad:
    f=bad[0]; print('first differing frame',f)
    for g in range(max(common[0],f-3),f+4):
        if g in a and g in b: print(g,'A',a[g],'\n   B',b[g])
