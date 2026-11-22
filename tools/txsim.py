"""Emulate the stock firmware's TX bit-bang state machine and print the waveform.

Runs the Thumb function at 0xcbd38 (the timer callback that shifts a frame out
on GPIO14) in a minimal interpreter, with the GPIO-write and timer-arm calls
stubbed, and prints the resulting HIGH/LOW segments. Used to establish the full
stock waveform (idle HIGH, per-frame trailer) - see ANALYSIS.md section 2.3.

Usage (run from the project root):
    python -I tools/txsim.py <old|new> <8-hex-digit frame> [json]

    old   SMAVISTA_Ceiling-Fan-v1.0.2.deblocked.bin
    new   SMAVISTA_Ceiling-Fan-v1.0.3.bin
    json  also dump the segments as a JSON line ("JSON:[[level, us], ...]")

Requires the `capstone` Python package. No code from the firmware is executed
natively; only the instructions this routine uses are interpreted.
"""
import capstone,re,struct,sys
which=sys.argv[1] if len(sys.argv)>1 else 'old'
if which=='old': img=open('SMAVISTA_Ceiling-Fan-v1.0.2.deblocked.bin','rb').read(); base=0; src=0xc1e2c; cb=0xcbd38
else:            img=open('SMAVISTA_Ceiling-Fan-v1.0.3.bin','rb').read();           base=0x10000; src=0xc1dbc; cb=None
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_THUMB); md.detail=True
def code_u32(a): return int.from_bytes(img[a-base:a-base+4],'little')
# find callback in new image by matching old prologue bytes
if cb is None:
    o=open('SMAVISTA_Ceiling-Fan-v1.0.2.deblocked.bin','rb').read()
    sig=o[0xcbd38:0xcbd38+10]; cb=base+img.find(sig)
# RAM
ram={}
def rd8(a): return ram[a] if a in ram else (img[a-base] if base<=a<base+len(img) else 0)
def wr8(a,v): ram[a]=v&0xff
def rdN(a,n): return sum(rd8(a+i)<<(8*i) for i in range(n))
def wrN(a,v,n):
    for i in range(n): wr8(a+i,(v>>(8*i))&0xff)
# init .data from flash
for i in range(0x22a8): ram[0x400100+i]=img[src+i-base]
S=0x400f72
# --- set up like the packet builder would (6 copies of a frame), pin=14
frame=bytes.fromhex(sys.argv[2]) if len(sys.argv)>2 else bytes.fromhex('2222 2b12'.replace(' ',''))
wr8(S+0x10,14)
for k in range(6):
    for j,b in enumerate(frame): wr8(S+0x19+4*k+j,b)
wrN(S+0x16,0,2); wrN(S+0x21a,24,2)
wr8(S+0x11,1); wr8(S+0x12,1)
events=[]   # (time_us, level)
t=0.0
R={}
def run_callback():
    global t
    r=[0]*16; r[13]=0x500000; r[14]=0xFFFFFFF1
    N=Z=C=V=0
    pc=cb; armed=None
    steps=0
    def setf(res,carry=None):
        nonlocal N,Z,C
        res&=0xffffffff; N=res>>31; Z=int(res==0)
        if carry is not None: C=carry
    while True:
        steps+=1
        if steps>2000: raise RuntimeError('loop')
        ins=list(md.disasm(img[pc-base:pc-base+4],pc))
        if not ins: raise RuntimeError('bad insn at %x'%pc)
        i=ins[0]; m=i.mnemonic; op=i.op_str; nxt=pc+i.size
        def reg(x): return {'sp':13,'lr':14,'pc':15,'ip':12,'sb':9,'sl':10,'fp':11}.get(x,None) if x in('sp','lr','pc','ip','sb','sl','fp') else int(x[1:])
        def val(x):
            x=x.strip()
            if x.startswith('#'): return int(x[1:],0)&0xffffffff
            return r[reg(x)]
        parts=[p.strip() for p in re.split(r',\s*(?![^\[]*\])',op)]
        def mem_ops():
            mm=re.match(r'(\w+), \[(\w+)(?:, (#?-?\w+))?\]',op)
            rt,rn,off=mm.group(1),mm.group(2),mm.group(3)
            o=0
            if off is not None: o=val(off) if off.startswith('#') else r[reg(off)]
            return reg(rt),(r[reg(rn)]+o)&0xffffffff
        if m=='push' or (m=='pop' and 'pc' not in op):
            pass
        elif m=='ldr' and 'pc' in op and '[pc' in op:
            rt=reg(parts[0]); off=int(re.search(r'#(0x[0-9a-f]+)',op).group(1),16); r[rt]=code_u32(((pc+4)&~3)+off)
        elif m in('ldr','ldrb','ldrh'):
            rt,a=mem_ops(); r[rt]=rdN(a,{'ldr':4,'ldrb':1,'ldrh':2}[m])
        elif m in('str','strb','strh'):
            rt,a=mem_ops(); wrN(a,r[rt],{'str':4,'strb':1,'strh':2}[m])
        elif m=='movs' or m=='mov':
            d=reg(parts[0]); v=val(parts[1]); r[d]=v
            if m=='movs': setf(v)
        elif m in('adds','subs'):
            d=reg(parts[0])
            if len(parts)==2: a,b=r[d],val(parts[1])
            else: a,b=val(parts[1]),val(parts[2])
            res=(a+b) if m=='adds' else (a-b)
            r[d]=res&0xffffffff; setf(res, int(res>=0 and res>0xffffffff) if m=='adds' else int(a>=b))
        elif m=='ands':
            d=reg(parts[0]); b=val(parts[1]) if len(parts)==2 else val(parts[2]); a=r[d] if len(parts)==2 else val(parts[1]); r[d]=a&b; setf(r[d])
        elif m=='orrs':
            d=reg(parts[0]); r[d]=r[d]|val(parts[1]); setf(r[d])
        elif m=='eors':
            d=reg(parts[0]); r[d]=r[d]^val(parts[1]); setf(r[d])
        elif m=='tst':
            setf(val(parts[0])&val(parts[1]))
        elif m=='cmp':
            a,b=val(parts[0]),val(parts[1]); res=a-b; setf(res,int(a>=b)); V=0
        elif m in('lsls','lsrs','asrs'):
            d=reg(parts[0])
            if len(parts)==3: a,s=val(parts[1]),val(parts[2])
            else: a,s=r[d],val(parts[1])
            if m=='lsls': res=(a<<s)&0xffffffff
            elif m=='lsrs': res=a>>s
            else:
                sa=a-(1<<32) if a>>31 else a; res=(sa>>min(s,31))&0xffffffff
            r[d]=res; setf(res)
        elif m=='b' :
            nxt=int(op[1:],16)
        elif m in('beq','bne','bhi','blo','bhs','bls','blt','bge','bgt','ble','bcs','bcc'):
            cond={'beq':Z==1,'bne':Z==0,'bhi':C==1 and Z==0,'blo':C==0,'bcc':C==0,'bhs':C==1,'bcs':C==1,'bls':C==0 or Z==1}.get(m)
            if cond is None: raise RuntimeError('cond '+m)
            if cond: nxt=int(op[1:],16)
        elif m=='bl':
            tgt=int(op[1:],16)&0xffffffff
            if tgt==0x4b108: events.append((t,r[1]&1)); 
            elif tgt==0x4b42c: armed=r[1]
            elif tgt==0x122d4:   # case_uqi helper: table at lr (=nxt), index r0
                tbl=nxt; idx=r[0]; off=rd8(tbl+idx); nxt=tbl+2*off
            else: raise RuntimeError('unknown call %x at %x'%(tgt,pc))
        elif m=='pop' and 'pc' in op: return armed
        elif m=='push': pass
        else: raise RuntimeError('unhandled %s %s at %x'%(m,op,pc))
        pc=nxt
# run until state==0 and no timer
t=0.0
for k in range(5000):
    st=rd8(S+0x11)
    armed=run_callback()
    if armed is None: break
    t+=armed
    if rd8(S+0x11)==0 and False: break
print('callbacks',k+1,'end state',rd8(S+0x11),'total time %.1f ms'%(t/1000))
# convert events to segments
segs=[]
for (t0,l0),(t1,l1) in zip(events,events[1:]+[(t,None)]): segs.append((l0,t1-t0))
print('segments',len(segs))
# print pattern with compressed bits
def desc(l,d): return '%s%d'%('H' if l else 'L',d)
print(' '.join(desc(l,d) for l,d in segs[:12]),' ... ')
# find first frame: after first LOW gap
big=[i for i,(l,d) in enumerate(segs) if l==0 and d>5000]
print('big LOW gaps at',big,[segs[i][1] for i in big])
a=big[0]; b=big[1] if len(big)>1 else len(segs)
fr=segs[a+1:b]
print('segments in frame 1 (between gaps):',len(fr))
print('first 6 :',' '.join(desc(l,d) for l,d in fr[:6]))
print('last 8  :',' '.join(desc(l,d) for l,d in fr[-8:]))
print('final segs:',' '.join(desc(l,d) for l,d in segs[-6:]))

print('---- decode of frame 1 (between first two gaps) ----')
bits=''
j=0
fr=segs[big[0]+1:big[1]]
for k in range(0,64,2):
    (l1,d1),(l2,d2)=fr[k],fr[k+1]
    bits+='1' if d1>500 else '0'
print('32 bits:',bits,'=',hex(int(bits,2)),' expected',frame.hex())
print('trailer after bit 32:',' '.join(desc(l,d) for l,d in fr[64:]))
print('burst structure: gap(%d) | 6 x [32 bit-pulses | trailer H%d L%d H%d | gap] ; ends HIGH' % (segs[big[0]][1],fr[64][1],fr[65][1],fr[66][1]))
print('total burst %.1f ms, edge count %d'%(t/1000,len(segs)))

if len(sys.argv) > 3 and sys.argv[3] == 'json':
    import json
    print('JSON:' + json.dumps([[int(l), int(d)] for l, d in segs]))
