#!/usr/bin/env python3
import math, random
# Writes assets/icon/rdr.ico (the exe's icon) and icon_1024.png here. Needs Pillow.
from PIL import Image, ImageDraw, ImageFilter, ImageChops
S=1024
def lerp(a,b,t): return tuple(int(a[i]+(b[i]-a[i])*t) for i in range(len(a)))
def base(S, texture=True):
    im=Image.new('RGBA',(S,S),(0,0,0,0))
    # rounded square, radial red gradient
    bg=Image.new('RGBA',(S,S))
    px=bg.load(); c=S/2
    for y in range(S):
        for x in range(S):
            d=math.hypot(x-c,y-c*0.9)/(S*0.72)
            px[x,y]=lerp((150,28,20,255),(48,8,6,255),min(1,d))+()
    mask=Image.new('L',(S,S),0); md=ImageDraw.Draw(mask)
    r=int(S*0.18); m=int(S*0.04)
    md.rounded_rectangle([m,m,S-m,S-m],r,fill=255)
    im.paste(bg,(0,0),mask)
    d=ImageDraw.Draw(im)
    # cream rim
    d.rounded_rectangle([m,m,S-m,S-m],r,outline=(232,214,178,255),width=int(S*0.022))
    d.rounded_rectangle([m+int(S*0.035),m+int(S*0.035),S-m-int(S*0.035),S-m-int(S*0.035)],int(r*0.8),outline=(20,4,3,200),width=max(1,int(S*0.008)))
    # revolver cylinder, end on
    cx,cy=S/2,S/2; R=S*0.33
    cream=(236,224,196,255); ink=(28,10,8,255)
    # flutes: cylinder outline as a 6-lobed shape
    pts=[]
    for i in range(720):
        a=2*math.pi*i/720
        lobe=0.06*max(0,math.cos(6*(a+math.pi/6)))**8
        rr=R*(1-lobe)
        pts.append((cx+rr*math.cos(a),cy+rr*math.sin(a)))
    d.polygon(pts,fill=ink)
    pts2=[(cx+(x-cx)*0.94,cy+(y-cy)*0.94) for x,y in pts]
    d.polygon(pts2,fill=cream)
    # six chambers
    for k in range(6):
        a=-math.pi/2+k*math.pi/3
        x=cx+R*0.56*math.cos(a); y=cy+R*0.56*math.sin(a); rr=R*0.24
        d.ellipse([x-rr,y-rr,x+rr,y+rr],fill=ink)
        rr2=rr*0.62
        d.ellipse([x-rr2,y-rr2,x+rr2,y+rr2],fill=(122,22,16,255))   # a round's red primer
        rr3=rr*0.22
        d.ellipse([x-rr3,y-rr3,x+rr3,y+rr3],fill=(60,10,8,255))
    # the centre: a diamond (the banner's)
    dd=R*0.2
    d.polygon([(cx,cy-dd),(cx+dd,cy),(cx,cy+dd),(cx-dd,cy)],fill=ink)
    dd2=dd*0.55
    d.polygon([(cx,cy-dd2),(cx+dd2,cy),(cx,cy+dd2),(cx-dd2,cy)],fill=cream)
    if texture:
        # worn: specks of the red showing through the cream, a little grain
        random.seed(7)
        wear=Image.new('L',(S,S),0); wd=ImageDraw.Draw(wear)
        for _ in range(900):
            x=random.uniform(cx-R,cx+R); y=random.uniform(cy-R,cy+R)
            if math.hypot(x-cx,y-cy)>R: continue
            s=random.uniform(1.5,6)*S/1024
            if random.random()<0.15:
                l=random.uniform(10,40)*S/1024; a=random.uniform(0,math.pi)
                wd.line([x,y,x+l*math.cos(a),y+l*math.sin(a)],fill=255,width=max(1,int(s*0.6)))
            else:
                wd.ellipse([x-s,y-s,x+s,y+s],fill=255)
        crimson=Image.new('RGBA',(S,S),(90,16,12,255))
        cream_mask=Image.new('L',(S,S),0)
        cpx=im.load(); cm=cream_mask.load()
        for y in range(S):
            for x in range(S):
                p=cpx[x,y]
                if p[0]>200 and p[1]>190: cm[x,y]=255
        wear=ImageChops.multiply(wear,cream_mask)
        im.paste(crimson,(0,0),wear)
    return im
big=base(S,True)
big.save('icon_1024.png')
sizes=[256,128,64,48,32,24,16]
imgs=[]
for s in sizes:
    src=big if s>=48 else base(S,False)
    imgs.append(src.resize((s,s),Image.LANCZOS))
import os
out=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','assets','icon','rdr.ico')
imgs[0].save(out,sizes=[(s,s) for s in sizes],append_images=imgs[1:])
# preview sheet
sheet=Image.new('RGBA',(256+128+64+48+32+24+16+80+20,280),(240,240,240,255))
x=10
for im in imgs:
    sheet.paste(im,(x,140-im.height//2),im); x+=im.width+10
sheet.save('preview.png')
dark=Image.new('RGBA',sheet.size,(32,32,32,255))
x=10
for im in imgs:
    dark.paste(im,(x,140-im.height//2),im); x+=im.width+10
s2=Image.new('RGBA',(sheet.width,560)); s2.paste(sheet,(0,0)); s2.paste(dark,(0,280)); s2.save('preview.png')
print('ok')
