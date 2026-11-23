# generate scripts/vs4_busy.txt: parity_vs menu path with four human ports, then scripted mashing on all ports
import sys
src=open(sys.argv[1]).read().split('\n@match\n')[0].rstrip('\n').split('\n')
out=[]
for l in src:
    out.append(l)
    if l.startswith('240 p=2'): out+=['240 p=3','240 p=4']
    if l.startswith('400 p=2'): out+=['400 p=3 sy=127','400 p=4 sx=-61 sy=127']
    if l.startswith('430 p=2'): out+=['430 p=3','430 p=4']
    if l.startswith('440 p=2'): out+=['440 p=3 A','440 p=4 A']
    if l.startswith('450 p=2'): out+=['450 p=3','450 p=4']
out.append('\n@match')
seed=12345
def rnd(n):
    global seed
    seed=(seed*1103515245+12345)&0x7fffffff
    return (seed>>8)%n
acts=['A','B','X','A sx=100','A sx=-100','B sx=100','B sx=-100','B sy=100','cx=127','cx=-127','cy=127','cy=-127','R','Z','X sx=80','A sy=-100','']
ev=[]
for f in range(130,7200,12):
    for p in range(1,5):
        k=rnd(4)
        if k==0: ev.append((f,p,'sx=%d'%(90 if rnd(2) else -90)))
        elif k==1: ev.append((f,p,acts[rnd(len(acts))])); ev.append((f+4,p,''))
        elif k==2: ev.append((f,p,'sx=%d sy=%d'%(rnd(255)-127,rnd(255)-127)))
        else: ev.append((f,p,''))
ev.sort(key=lambda e:(e[0],e[1]))
for f,p,a in ev: out.append(('%d p=%d %s'%(f,p,a)).rstrip())
open(sys.argv[2],'w').write('\n'.join(out)+'\n')
