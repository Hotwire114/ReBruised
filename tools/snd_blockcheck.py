import sys; sys.path.insert(0,'tools')
from snd_compare_lib import load
import numpy as np, collections
cap=sys.argv[1]; refp=sys.argv[2]; cs=float(sys.argv[3]); rs=float(sys.argv[4])
c,_=load(cap); r,_=load(refp)
# find exact offset by small search around (cs, rs) using shifted-by-53 hypothesis on L samples 0..150 of blocks
N=48000*2
s0=int(cs*48000)
best=None
from scipy import signal
x=c[s0:s0+N,0]; R=r[int(rs*48000)-96000:int(rs*48000)+96000+N,0]
cc=signal.fftconvolve(R,x[::-1],mode='valid'); k=int(np.argmax(cc)); roff=int(rs*48000)-96000+k
off=s0-roff
x=c[s0:s0+N]; y=r[roff:roff+N]
for ch in (0,1):
    bad=np.nonzero(np.abs(x[:,ch]-y[:,ch])>8)[0]
    h=collections.Counter(((roff+p)%256) for p in bad)
    print('ch',ch,'offset',off,'bad',len(bad),'of',N,'in-block range',(min(h),max(h)) if h else None)
