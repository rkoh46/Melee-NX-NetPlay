#!/usr/bin/env python3
"""Row-by-row, column-by-column comparison of two state digest CSVs. usage: compare_digests.py reference.csv other.csv
Float columns hold raw IEEE-754 bit patterns; differences are reported in ULPs."""
# SPDX-License-Identifier: GPL-3.0-or-later
import sys,struct
a=[l.rstrip('\n').split(',') for l in open(sys.argv[1])]
b=[l.rstrip('\n').split(',') for l in open(sys.argv[2])]
print('rows',len(a),len(b))
names=['retrace','scene_major','scene','match_frame','rng']+[f'p{p}.{f}' for p in range(1,7) for f in 'present stocks action anim_frame pos_x pos_y pos_z vel_x vel_y vel_z percent facing'.split()][:len(a[0])-5]
FL={'anim_frame','pos_x','pos_y','pos_z','vel_x','vel_y','vel_z','percent','facing'}
def f(u): return struct.unpack('<f',struct.pack('<I',int(u)&0xffffffff))[0]
def ulp(x,y):
    def k(u):
        u=int(u)&0xffffffff
        return u if u<0x80000000 else 0x80000000-u
    return abs(k(x)-k(y))
first=None; nin=nout=0; cols={}
for i,(r,s) in enumerate(zip(a,b)):
    d=[j for j in range(min(len(r),len(s))) if r[j]!=s[j]]
    if len(r)!=len(s): d.append(-1)
    if d:
        if first is None: first=(i,d)
        if r[1]=='2' and r[2]=='2': nin+=1
        else: nout+=1
        for j in d: cols[names[j]]=cols.get(names[j],0)+1
print('differing rows: inside a VS match (scene 2/2)',nin,'outside',nout)
if first:
    i,d=first; print('first differing row',i+1,'(retrace',a[i][0],') columns',[(names[j],a[i][j],b[i][j]) + ((f(a[i][j]),f(b[i][j]),'ulp=%d'%ulp(a[i][j],b[i][j])) if names[j].split('.')[-1] in FL else ()) for j in d[:8]])
    print('column diff counts',sorted(cols.items(),key=lambda x:-x[1])[:20])
else: print('IDENTICAL')
