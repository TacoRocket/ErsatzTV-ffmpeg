"""Fixed HDR10 Hable LUT, using FFmpeg's OpenCL math; no scene analysis.

Layout follows Intel libva-utils' 65/65/128, 16-bit four-channel LUT sample.
This diagnostic writes a raw hardware table, not a portable .cube file.
"""
from pathlib import Path
import numpy as np

root=Path(__file__).resolve().parent
size=65
v=np.linspace(0.0,1.0,size)
r,g,b=np.meshgrid(v,v,v,indexing='ij')
rgb=np.stack((r,g,b),axis=-1)

def rgb_to_xyz(primaries):
    xyz=np.array([(x/y,1,(1-x-y)/y) for x,y in primaries]).T
    white=np.array([0.3127/0.3290,1,(1-0.3127-0.3290)/0.3290])
    return xyz*np.linalg.solve(xyz,white)

m2020=rgb_to_xyz([(0.708,0.292),(0.170,0.797),(0.131,0.046)])
m709=rgb_to_xyz([(0.640,0.330),(0.300,0.600),(0.150,0.060)])
conversion=np.linalg.solve(m709,m2020)
# SMPTE ST 2084: result relative to FFmpeg's 100-nit reference white.
m1=2610/16384
m2=2523/32
c1=3424/4096
c2=2413/128
c3=2392/128
p=rgb**(1/m2)
linear=(np.maximum(p-c1,0)/np.maximum(c2-c3*p,1e-6))**(1/m1)*100
linear=linear@conversion.T
sig=np.maximum(linear.max(axis=-1),1e-6)
def hable(x):
    return (x*(x*0.15+0.50*0.10)+0.20*0.02)/(x*(x*0.15+0.50)+0.20*0.30)-0.02/0.30
mapped=np.minimum(hable(sig)/hable(10.0),1)
linear*= (mapped/sig)[...,None]
output=np.maximum(linear,0)**(1/2.4)

def save(name, values):
    table=np.zeros((65,65,128,4),dtype='<u2')
    table[:,:,:65,:3]=np.rint(np.clip(values,0,1)*65535).astype('<u2')
    table[:,:,65:,:]=table[:,:,64:65,:]
    (root/name).write_bytes(table.tobytes())
    print(name,table.nbytes)
save('hable65.bin',output)
save('identity65.bin',rgb)
