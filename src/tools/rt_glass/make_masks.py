import numpy as np, pickle, os, re, shutil
from PIL import Image, ImageDraw
from scipy import ndimage as ndi
D='/tmp/claude-0/-home-claude/d3d70956-fa42-5b72-b65b-492f583a0b7a/scratchpad/glass'
masks,how,src=pickle.load(open(D+'/masks.pkl','rb'))
S=256
yy,xx=np.mgrid[0:S,0:S]; X=(xx+0.5)/S*8; Y=(yy+0.5)/S*8
def rect(x0,y0,x1,y1): return (X>=x0)&(X<=x1)&(Y>=y0)&(Y<=y1)
def arch(x0,y0,x1,y1):
    r=(x1-x0)/2; cx=(x0+x1)/2; cy=y0+r
    return rect(x0,cy,x1,y1)|(((X-cx)**2+(Y-cy)**2<=r*r)&(Y<=cy))
def pointed(x0,y0,x1,y1):
    w=x1-x0; ys=y0+0.866*w
    top=((X-x0)**2+(Y-ys)**2<=w*w)&((X-x1)**2+(Y-ys)**2<=w*w)&(Y<=ys)&(X>=x0)&(X<=x1)
    return rect(x0,ys,x1,y1)|top
EMPTY=np.zeros((S,S),bool)
def lum(a): return 0.21*a[...,0]+0.72*a[...,1]+0.07*a[...,2]
def pairmask(b,l):
    A=np.asarray(Image.open(b).convert('RGB').resize((S,S),Image.BILINEAR)).astype(np.float32)/255
    B=np.asarray(Image.open(l).convert('RGB').resize((S,S),Image.BILINEAR)).astype(np.float32)/255
    m=((lum(B)-lum(A))>0.12)|(((B[...,0]-B[...,2])-(A[...,0]-A[...,2]))>0.15)
    m=ndi.binary_opening(m,structure=np.ones((3,3)))
    m=ndi.binary_closing(m,structure=np.ones((7,7)))
    m=ndi.binary_fill_holes(m)
    lab,n=ndi.label(m)
    if n:
        s=ndi.sum(m,lab,range(1,n+1)); m=np.isin(lab,1+np.nonzero(s>=0.004*m.size)[0])
    return m
def litmask(f):
    a=np.asarray(Image.open(f).convert('RGBA').resize((S,S),Image.BILINEAR)).astype(np.float32)
    r,g,b,al=[a[...,i] for i in range(4)]
    m=(al>=128)&((54*r+183*g+19*b)/256>100)&(r>=b+25)&(g>=b)
    m=ndi.binary_closing(m,structure=np.ones((11,11))); m=ndi.binary_fill_holes(m)
    m=ndi.binary_opening(m,structure=np.ones((3,3)))
    return m
out={}
FULL=set()
# pairs (stock families, HD pixels)
P=[('win11','win11l'),('win31','win31l'),('win5','win5l'),('win51','win51l'),('win52','win52l'),('winwd264','winwl264'),('winwd64','winwl64'),('cfwinf01','cfwinl01'),('win323','win323l'),('win32','win32l'),('win322','win322l')]
for b,l in P:
    m=pairmask(src['fam/city/'+b],src['fam/city/'+l]); out['fam/city/'+b]=m; out['fam/city/'+l]=m
for k in ['fam/city/win324','fam/city/win324l','fam/city/win325','fam/city/win325l']: FULL.add(k)
m=pointed(1.1,1.2,3.6,6.3)|pointed(4.0,1.2,6.6,6.3); out['fam/city/win1']=m; out['fam/city/win1l']=m
shapes={
 'fam/city/win2':arch(1.9,0.4,6.1,6.6),'fam/temple/window':arch(1.9,0.4,6.1,6.6),
 'fam/city/win3':pointed(2.8,1.3,5.7,4.9),'fam/city/win4':rect(3.0,0.2,5.4,7.0),'fam/city/win6':rect(2.9,0.6,5.8,6.6),
 'fam/city/win326l':rect(3.2,1.2,5.6,5.4),'fam/temple/windowin':pointed(0.3,0.6,7.7,8.0),
 'fam/buriedcity/window1':arch(3.0,2.4,4.7,5.4),
 'fam/lostcty/winwal01':rect(2.6,0.4,5.4,5.3),'fam/lostcty/winwal02':rect(2.6,0.4,5.4,5.3),
 'fam/lostcty/winwal03':rect(2.85,0.3,5.25,4.4),'fam/lostcty/winwal04':rect(2.8,0.3,5.3,4.5),
 'fam/lostcty/winwal05':rect(2.6,0.5,5.4,5.6),'fam/lostcty/winwal06':rect(2.4,0.4,5.2,2.0)|rect(2.4,3.1,5.2,5.0),
 'fam/ramirez/window128':EMPTY,'fam/ramirez/window128base':EMPTY,'fam/ramirez/windowbl':EMPTY,
 'fam/skcity/windowchurch01':pointed(0.3,0.6,3.6,8.0)|pointed(4.4,0.6,7.7,8.0),
 'fam/sqtxt/windowwood01':rect(0,0,8,4.6),'fam/sqtxt/windowwood02':rect(0,0,8,4.6),
 'fam/skcity/windowwood01':rect(0,0,8,4.6),'fam/skcity/windowwood02':rect(0,0,8,4.6),
 'fam/city/cfwinl01base':rect(1.2,3.2,3.7,6.6)|rect(4.2,3.2,6.8,6.6),
}
two=pointed(1.2,1.3,3.7,6.2)|pointed(4.2,1.3,6.7,6.2)
shapes.update({
 'fam/city/win51':pointed(2.3,1.3,5.8,6.0),'fam/city/win51l':pointed(2.3,1.3,5.8,6.0),
 'fam/city/win5':arch(2.6,1.4,5.4,6.0),'fam/city/win5l':arch(2.6,1.4,5.4,6.0),
 'fam/city/win31':pointed(2.7,1.2,5.0,6.6),'fam/city/win31l':pointed(2.7,1.2,5.0,6.6),
 'fam/sktxt/win52lit':pointed(2.6,1.6,5.4,6.2),'fam/sktxt/win5lit':pointed(2.6,1.4,5.5,6.3),
 'fam/sktxt/win1lit':two,'fam/sktxt/win11lit':two,
 'fam/dirkbogantxt/rwind03lit':arch(0.9,1.5,3.8,7.2)|arch(4.2,1.5,7.1,7.2),
})
out.update(shapes)
# everything else: whole texture glass (no mask file)
for k in masks:
    if k not in out: FULL.add(k)
shutil.rmtree(D+'/out',ignore_errors=True); os.makedirs(D+'/out')
safe=lambda k: re.sub(r'[^a-z0-9-]','_',k.lower())
for k,m in out.items():
    Image.fromarray((m*255).astype(np.uint8),'L').save(D+'/out/'+safe(k)+'.png',optimize=True)
print(len(out),'masks;',len(FULL),'whole glass:',sorted(FULL))
tiles=[]
for k in sorted(out):
    im=Image.open(src[k]).convert('RGB').resize((128,128)); mm=np.asarray(Image.fromarray(out[k].astype(np.uint8)*255).resize((128,128)))>127
    ov=np.asarray(im).copy(); ov[mm]=(0.45*ov[mm]+0.55*np.array([0,255,255])).astype(np.uint8)
    tiles.append((k,im,Image.fromarray(ov)))
cols=8
M=Image.new('RGB',(cols*262,((len(tiles)+cols-1)//cols)*142),(255,0,255)); d=ImageDraw.Draw(M)
for i,(k,a,o) in enumerate(tiles):
    x,y=(i%cols)*262,(i//cols)*142; M.paste(a,(x,y)); M.paste(o,(x+130,y)); d.text((x+2,y+129),k.replace('fam/',''),fill=(255,255,255))
M.save(D+'/final.png')
