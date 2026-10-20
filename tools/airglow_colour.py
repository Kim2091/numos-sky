# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

# Colour of the natural night-sky airglow at the zenith (linear sRGB, unit luminance) from a typical
# solar-mean spectrum: lines in Rayleighs plus a pseudo-continuum in R/A (Broadfoot & Kendall 1968,
# Leinert et al. 1998 sec. 7, Roach & Gordon 1973).
# The result is kAirglowColour in src/renderer.cpp.
import numpy as np
def g(x,mu,s1,s2): return np.exp(-0.5*((x-mu)/np.where(x<mu,s1,s2))**2)
def cmf(l):  # Wyman, Sloan & Shirley 2013 multi-lobe fit, nm
    x=1.056*g(l,599.8,37.9,31.0)+0.362*g(l,442.0,16.0,26.7)-0.065*g(l,501.1,20.4,26.2)
    y=0.821*g(l,568.8,46.9,40.5)+0.286*g(l,530.9,16.3,31.1)
    z=1.217*g(l,437.0,11.8,36.0)+0.681*g(l,459.0,26.0,13.8)
    return np.stack([x,y,z],-1)
M=np.array([[3.2406,-1.5372,-0.4986],[-0.9689,1.8758,0.0415],[0.0557,-0.2040,1.0570]])
lines={557.7:250,589.3:50,630.0:60,636.4:20}
# OH Meinel bands inside the visible (6-1 to 9-4 heads 650-780 nm), ~100 R per 10 nm over 600-700.
lam=np.arange(380.,781.,1.)
cont=np.interp(lam,[380,450,500,550,600,650,700,780],[0.3,0.4,0.5,0.55,0.6,0.65,0.7,0.7])*10  # R/nm
oh=np.where(lam>=600,np.interp(lam,[600,650,700,780],[1,3,6,10]),0.)  # OH (6-1)..(9-4) above the continuum
photons=cont+oh
XYZ=(cmf(lam)*(photons/lam)[:,None]).sum(0)
for l,r in lines.items(): XYZ+=cmf(np.array([l]))[0]*r/l
rgb=M@XYZ; Y=rgb@np.array([0.2126,0.7152,0.0722])
print('airglow rgb (unit luminance):',np.round(rgb/Y,4))
