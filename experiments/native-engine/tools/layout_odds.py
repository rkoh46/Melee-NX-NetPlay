#!/usr/bin/env python3
# How often does Horizon put a region over the game's fixed ranges? A Monte Carlo of the kernel's
# region placement for a 39-bit process with ASLR (Atmosphere, KPageTableBase::InitializeForProcess,
# and the loader's choice of the code address). Usage: layout_odds.py [LAUNCHES]
# SPDX-License-Identifier: GPL-3.0-or-later
import random, sys
GB=1<<30; MB=1<<20; A=2*MB
START=128*MB; SIZE=512*GB-128*MB; END=START+SIZE
REG={'kmap':64*GB,'stack':2*GB,'alias':64*GB,'heap':8*GB}
ORDER=['kmap','stack','alias','heap']           # insertion order; selection sort largest first (stable enough: swap on <)
WANT=[(0x80000000,0x84000000),(0xE0000000,0xE0004000)]
def layout(code_size, rnd):
    free=SIZE-code_size
    code=START+rnd.randrange(free//A)*A      # loader: GenerateSecureRandom(n) taken as [0,n)
    pcs=code//A*A; pce=-(-(code+code_size)//A)*A
    regs=[[REG[n],n,0] for n in ORDER]
    n=len(regs)
    for i in range(n-1):
        for j in range(i+1,n):
            if regs[i][0]<regs[j][0]: regs[i],regs[j]=regs[j],regs[i]
    starts=[START,pce]; sizes=[pcs-START,END-pce]; counts=[0,0]
    for r in regs:
        r[2]=1 if sizes[1]>=sizes[0] else 0; counts[r[2]]+=1; sizes[r[2]]-=r[0]
    for i in range(n-1):
        for j in range(i+1,n):
            if regs[i][2]>regs[j][2]: regs[i],regs[j]=regs[j],regs[i]
    out={'code':(code,code+code_size)}
    for side in (0,1):
        c=counts[side]
        if not c: continue
        first=next(i for i,r in enumerate(regs) if r[2]==side)
        for i in range(c-1):
            k=rnd.randint(i,c-1); regs[first+i],regs[first+k]=regs[first+k],regs[first+i]
        offs=sorted(rnd.randint(0,sizes[side]//A)*A for _ in range(c))
        prev_end=starts[side]; prev_off=0
        for i in range(c):
            r=regs[first+i]; s=offs[i]-prev_off+prev_end
            out[r[1]]=(s,s+r[0]); prev_end=s+r[0]; prev_off=offs[i]
    return out
def hit(rg): return any(rg[0]<e and s<rg[1] for s,e in WANT)
N=int(sys.argv[1]) if len(sys.argv) > 1 else 1000000; rnd=random.Random(1)
cnt={k:0 for k in ['heap','alias','stack','kmap','code','heap_or_alias','heap_cache_only']}
for _ in range(N):
    L=layout(8*MB,rnd)
    h=hit(L['heap']); a=hit(L['alias'])
    for k in ('heap','alias','stack','kmap','code'): cnt[k]+=hit(L[k])
    cnt['heap_or_alias']+=h or a
    cnt['heap_cache_only']+= h and L['heap'][0]>=0x84000000
for k,v in cnt.items(): print(f"{k:16s} {v:8d}  {100*v/N:.3f}%  1 in {N/v if v else float('inf'):.0f}")
