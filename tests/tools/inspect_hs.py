import sys
from pathlib import Path as _P
sys.path.insert(0, str(_P(__file__).resolve().parents[1]))
from qa_common import *  # noqa: E402,F403
from collections import Counter
from pathlib import Path
import mmap,sys,struct
sys.path.insert(0, str(root / 'tools'))
from import_halo_a30 import HaloMap
ids='a10 a30 a50 b30 b40 c10 c20 c40 d20 d40'.split()
for id in (sys.argv[1:2] or ids):
 with (HALO_SOURCE / f'{id}.map').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as m:
  h=HaloMap(m); t=h.tags;a=h.scenario
  p=t.read('I',a+1152)[0]; s=t.read('I',a+1172)[0]; ln=t.read('I',a+1160)[0]
  roots=[(t.name(b,32),*t.read('HHI',b+32)) for b in t.block(a+1180,92)]
  glob=[t.name(b,32) for b in t.block(a+1192,92)]
  nodes=[t.read('4H3I',p+56+20*i) for i in range((t.read('I',a+1140)[0]-56)//20)]
  def node(i):
   result=nodes[i&65535]
   assert i>>16==result[0], (i,result)
   return result
  def name(i):
   return t.name(s+i,256) if i<ln else str(i)
  def ex(i,depth=0):
   _, op, ty, flag, nxt, st, data=node(i)
   if flag&1:
    if flag&4: return glob[op] if op<len(glob) else f'<global:{op}:{name(st)}>'
    if op==6: return str(round(struct.unpack('<f',struct.pack('<I',data))[0],5))
    if op==5: return 'true' if data&255 else 'false'
    if op==7: return str(struct.unpack('<h',struct.pack('<H',data&65535))[0])
    if op==8: return str(struct.unpack('<i',struct.pack('<I',data))[0])
    if op==10: return '&'+roots[data&65535][0]
    if op==24 and data&65535<len(h.directory): return name(st)+f'#{data&65535}'
    return name(st)
   args=[]
   while data!=4294967295:
    args.append(ex(data,depth+1));data=node(data)[4]
   if depth==0 or len(' '.join(args))>150:
    return '('+'\n'+'  '*(depth+1)+('\n'+'  '*(depth+1)).join(args)+'\n'+'  '*depth+')'
   return '('+' '.join(args)+')'
  if len(sys.argv)>2:
   for i,(n,k,ty,root) in enumerate(roots):
    if any(x==n for x in sys.argv[2:]):
     print(n,k,ex(root))
  else:
   print(id,'STARTUP',[(n) for n,k,_,_ in roots if k==0])
   print(id,'SCENES',[(n) for n,k,_,_ in roots if n.startswith(('x','cin','mission_','intro','outro','insertion','extraction'))][:80])
   for _,op,ty,flag,_,st,data in nodes:
    if flag&1 and op==2 and name(st) in ('camera_set','cinematic_start','switch_bsp','sound_impulse_start'):
     pass
