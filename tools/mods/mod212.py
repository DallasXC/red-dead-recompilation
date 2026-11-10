#!/usr/bin/env python3
"""
mod212.py - the "version: 2.12" binary model (.mod / .xmod), as the game's
loader (sub_00183440) reads it: the text format's fields in order, without
their names.

  "version: 2.12\0"  11 u32: verts normals colors tex1s tex2s tangents
                      materials adjuncts primitives matrices reskins
  verts float3, normals float3, colors float4, tex1s float2, tex2s float2,
  tangents float3
  materials: name " ", u32 packets, primitives, textures, illum,
             12 floats (ambient rgb, diffuse rgb, specular rgb, ...),
             textures names " ", u32 attributes
  per material, its packets: u16 adjuncts, primitives, matrices, reskins;
             adjuncts 6 u16 (vert normal color tex1 tex2 matrix-slot);
             reskins u16 adjunct, u16 slot, f32 weight, f32 x y z;
             primitives u8 type (1 strip, 2 strip with a flipped start),
             u8 count, count u8 adjunct indices; matrices u8 bone each

The January 2002 preview's character models keep each vertex relative to
its bone (the bind pose has no rotations: a bone's place is the sum of the
offsets down to it in <name>.skel); the retail game wants them in the
model's space. to_model_space() adds each vertex's bone position (checked
against the retail disc's own text copy of the preview's Red: 2767 of 2799
corners agree to a millimetre; no vertex is shared by two bones). A vertex
blended between bones (its adjunct's slot past the packet's matrices; the
long-coat Red, coc) is put at its reskins' weighted place.

  python tools\\mods\\mod212.py convert <in.mod> <name.skel> <out.xmod>
"""
import struct,sys,re
def parse(d):
    assert d[:13]==b'version: 2.12' or d[:11]==b'version: 2.', d[:14]
    o=14
    (nv,nn,nc,nt1,nt2,ntg,nmat,nadj,nprim,nmtx,nres)=struct.unpack_from('<11I',d,o); o+=44
    m={}
    m['v']=[struct.unpack_from('<3f',d,o+12*i) for i in range(nv)]; o+=12*nv
    m['n']=[struct.unpack_from('<3f',d,o+12*i) for i in range(nn)]; o+=12*nn
    m['c']=[struct.unpack_from('<4f',d,o+16*i) for i in range(nc)]; o+=16*nc
    m['t1']=[struct.unpack_from('<2f',d,o+8*i) for i in range(nt1)]; o+=8*nt1
    m['t2']=[struct.unpack_from('<2f',d,o+8*i) for i in range(nt2)]; o+=8*nt2
    o+=12*ntg
    mats=[]
    for i in range(nmat):
        e=d.index(b' ',o); name=d[o:e].decode(); o=e+1
        npk,npr,a,b=struct.unpack_from('<4I',d,o); o+=16
        fl=struct.unpack_from('<12f',d,o); o+=48
        texs=[]
        for t in range(a):
            e=d.index(b' ',o); texs.append(d[o:e].decode()); o=e+1
        attr=struct.unpack_from('<I',d,o)[0]; o+=4
        mats.append(dict(name=name,packets=npk,prims=npr,ntex=a,illum=b,fl=fl,tex=texs,attr=attr))
    m['mats']=mats
    pk=[]
    for mi,mt in enumerate(mats):
        for p in range(mt['packets']):
            na,npr,nmx,nrs=struct.unpack_from('<4H',d,o); o+=8
            adj=[struct.unpack_from('<6H',d,o+12*i) for i in range(na)]; o+=12*na
            rs=[]; rso=[]
            for i in range(nrs):
                a,b=struct.unpack_from('<2H',d,o); w=struct.unpack_from('<4f',d,o+4); rs.append((a,b)+w); rso.append(o); o+=20
            prims=[]
            for i in range(npr):
                t,c=d[o],d[o+1]; idx=list(d[o+2:o+2+c]); prims.append((t,idx)); o+=2+c
            mtx=list(d[o:o+nmx]); mto=o; o+=nmx
            pk.append(dict(mat=mi,adj=adj,rs=rs,rs_off=rso,prims=prims,mtx=mtx,mtx_off=mto))
    m['packets']=pk
    m['end']=o; m['len']=len(d)
    m['counts']=(nv,nn,nc,nt1,nt2,ntg,nmat,nadj,nprim,nmtx,nres)
    return m
def parse200(d):
    """The older "version: 2.00" model (the preview's Old): ten counts (no
    reskins), materials without the attributes word, and every packet
    field a u32 (header adjuncts primitives matrices reskins; adjuncts 6
    u32; a primitive u32 type, u32 count, count u32 indices; matrices u32
    each); an "mtxv" table follows. Its vertices are already in the
    model's space (Old: 1.65 m tall from them as they are). Same shape as
    parse()'s result, for drawing (charpic.py)."""
    assert d[:13] == b'version: 2.00', d[:14]
    o = 14
    nv, nn, nc, nt1, nt2, ntg, nmat, nadj, nprim, nmtx = struct.unpack_from('<10I', d, o); o += 40
    m = {}
    m['v'] = [struct.unpack_from('<3f', d, o + 12 * i) for i in range(nv)]; o += 12 * nv
    m['n'] = [struct.unpack_from('<3f', d, o + 12 * i) for i in range(nn)]; o += 12 * nn
    m['c'] = [struct.unpack_from('<4f', d, o + 16 * i) for i in range(nc)]; o += 16 * nc
    m['t1'] = [struct.unpack_from('<2f', d, o + 8 * i) for i in range(nt1)]; o += 8 * nt1
    m['t2'] = [struct.unpack_from('<2f', d, o + 8 * i) for i in range(nt2)]; o += 8 * nt2
    o += 12 * ntg
    mats = []
    for i in range(nmat):
        e = d.index(b' ', o); name = d[o:e].decode('latin-1'); o = e + 1
        npk, npr, a, b = struct.unpack_from('<4I', d, o); o += 16
        fl = struct.unpack_from('<12f', d, o); o += 48
        texs = []
        for t in range(a):
            e = d.index(b' ', o); texs.append(d[o:e].decode('latin-1')); o = e + 1
        mats.append(dict(name=name, packets=npk, prims=npr, ntex=a, illum=b, fl=fl, tex=texs, attr=0))
    m['mats'] = mats
    pk = []
    for mi, mt in enumerate(mats):
        for p in range(mt['packets']):
            na, npr, nmx, nrs = struct.unpack_from('<4I', d, o); o += 16
            adj = [struct.unpack_from('<6I', d, o + 24 * i) for i in range(na)]; o += 24 * na
            o += 20 * nrs
            prims = []
            for i in range(npr):
                t, c = struct.unpack_from('<2I', d, o); o += 8
                prims.append((t, list(struct.unpack_from('<%dI' % c, d, o)))); o += 4 * c
            mtx = list(struct.unpack_from('<%dI' % nmx, d, o)); o += 4 * nmx
            pk.append(dict(mat=mi, adj=adj, rs=[], rs_off=[], prims=prims, mtx=mtx, mtx_off=0))
    m['packets'] = pk
    m['end'] = o; m['len'] = len(d)
    m['counts'] = (nv, nn, nc, nt1, nt2, ntg, nmat, nadj, nprim, nmtx, 0)
    return m


def skel_world(text):
    bones=[]; stack=[]
    for b,ox,oy,oz,close in re.findall(r'bone\s+(\S+)\s*\{|offset\s+(\S+)\s+(\S+)\s+(\S+)|(\})', text):
        if b:
            bones.append([b, stack[-1] if stack else -1, (0.0,0.0,0.0)]); stack.append(len(bones)-1)
        elif ox:
            bones[stack[-1]][2]=(float(ox),float(oy),float(oz))
        elif close and stack:
            stack.pop()
    world=[]
    for n,p,o in bones:
        if p<0: world.append(o)
        else:
            w=world[p]; world.append((w[0]+o[0],w[1]+o[1],w[2]+o[2]))
    return world

STD_BONES = ["root", "spine_01", "spine_02", "neck_01", "head"] + [
    b + s for s in ("_l", "_r") for b in (
        "clavicle", "shoulder", "elbow", "wrist", "thumb_01", "thumb_02", "thumb_03",
        "finger_11", "finger_12", "finger_13", "finger_21", "finger_22", "finger_23",
        "finger_31", "finger_32", "finger_33", "finger_41", "finger_42", "finger_43")] + [
    "pelvis", "hip_l", "knee_l", "ankle_l", "toe_l", "hip_r", "knee_r", "ankle_r", "toe_r"]


STD_PARENT = {"root": None, "spine_01": "root", "spine_02": "spine_01", "neck_01": "spine_02",
              "head": "neck_01", "pelvis": "root"}
for _s in ("_l", "_r"):
    STD_PARENT.update({"clavicle" + _s: "spine_02", "shoulder" + _s: "clavicle" + _s,
                       "elbow" + _s: "shoulder" + _s, "wrist" + _s: "elbow" + _s,
                       "thumb_01" + _s: "wrist" + _s, "thumb_02" + _s: "thumb_01" + _s,
                       "thumb_03" + _s: "thumb_02" + _s,
                       "hip" + _s: "pelvis", "knee" + _s: "hip" + _s, "ankle" + _s: "knee" + _s,
                       "toe" + _s: "ankle" + _s})
    for _f in "1234":
        STD_PARENT["finger_%s1%s" % (_f, _s)] = "wrist" + _s
        STD_PARENT["finger_%s2%s" % (_f, _s)] = "finger_%s1%s" % (_f, _s)
        STD_PARENT["finger_%s3%s" % (_f, _s)] = "finger_%s2%s" % (_f, _s)
# where a bone missing from a preview skeleton goes (the retail cls's)
STD_OFFSET = {"thumb_01_l": (0.024, -0.043, -0.039), "thumb_01_r": (-0.024, -0.043, -0.039),
              "toe_l": (0.0, -0.12, -0.142), "toe_r": (0.0, -0.12, -0.142)}


def skel_bones(text):
    """[(name, parent index, offset)] in the file's order, and world places."""
    bones = []; stack = []
    for b, ox, oy, oz, close in re.findall(r"bone\s+(\S+)\s*\{|offset\s+(\S+)\s+(\S+)\s+(\S+)|(\})", text):
        if b:
            bones.append([b, stack[-1] if stack else -1, (0.0, 0.0, 0.0)]); stack.append(len(bones) - 1)
        elif ox:
            bones[stack[-1]][2] = (float(ox), float(oy), float(oz))
        elif close and stack:
            stack.pop()
    world = []
    for n, p, o in bones:
        w = world[p] if p >= 0 else (0.0, 0.0, 0.0)
        world.append((w[0] + o[0], w[1] + o[1], w[2] + o[2]))
    return bones, world


def drop_bones(text, drop, first=False):
    """The skeleton without the bones named in drop: their children hang
    from their parents, their offsets added to the children's. first: they
    come before the parent's own children (in the numbering too)."""
    bones, _ = skel_bones(text)
    keep = [i for i, b in enumerate(bones) if b[0] not in drop]
    parent = {}
    offset = {}
    for i, (n, p, o) in enumerate(bones):
        q, off = p, o
        while q >= 0 and bones[q][0] in drop:
            po = bones[q][2]
            off = (off[0] + po[0], off[1] + po[1], off[2] + po[2])
            q = bones[q][1]
        parent[i], offset[i] = q, off
    kids = {}
    for i in keep:
        kids.setdefault(parent[i], []).append(i)
    if first:
        for q in kids:
            kids[q] = ([i for i in kids[q] if bones[i][1] != q] + [i for i in kids[q] if bones[i][1] == q])
    out = ["NumBones %d" % len(keep)]
    def emit(i, depth):
        t = "\t" * depth
        o = offset[i]
        out.append("%sbone %s {" % (t, bones[i][0]))
        out.append("%s\toffset %f %f %f" % (t, o[0], o[1], o[2]))
        for c in kids.get(i, []):
            emit(c, depth + 1)
        out.append("%s}" % t)
    for r in kids.get(-1, []):
        emit(r, 0)
    return "\n".join(out) + "\n"


# Bones a skeleton may carry that its models do not count: the preview's
# cloth anchors (the long-coat Red, coc: 94 bones in coc.skel, 92 in the
# model's numbering -- its toes came out half a metre under the ground).
CLOTH_ANCHORS = ("root_coat", "coattail")


def fit_skeleton(skel_text, model):
    """The skeleton as the model numbers its bones: without the cloth
    anchors when the model counts that many fewer."""
    try:
        m = parse(model)
    except Exception:
        return skel_text
    used = max((b for p in m["packets"] for b in p["mtx"]), default=-1) + 1
    bones, _ = skel_bones(skel_text)
    names = [b[0] for b in bones]
    extra = [n for n in CLOTH_ANCHORS if n in names]
    if used <= len(bones) - len(extra) and extra and used > len(bones) - len(extra) - 8:
        # The cloth's bones come first among the anchor's parent's
        # children in the model's numbering: the long-coat Red's coat
        # (40 bones, hung from root_coat) is 3..42 and his neck and head
        # 43 and 44 -- numbered as the file lists them, the head went to
        # a coat tail's bone and was drawn at the knees (user screenshot;
        # 228 vertices on "f_l4", 3 on "head").
        return drop_bones(skel_text, set(extra), first=True)
    return skel_text


def std_skeleton(text):
    """A preview skeleton whose bones are not the retail 52 in order, as the
    retail 52 in order (version 100, as the others), with the preview's own
    proportions; and, per old bone, the new bone its vertices go to (an
    extra -- Christina's hair, a scarf, a coat's tails -- goes to its
    nearest retail ancestor) and how far that one is from it. The old
    format hands the animations' channels out by a bone's place in the
    file: extras before some of the 52 gave those the wrong channels (her
    arms folded; Young and Teen Red's legs off the ground). None when it is
    already in order (extras only after the 52) or lacks its root."""
    bones, world = skel_bones(text)
    names = [b[0] for b in bones]
    if all(i < len(names) and (names[i] == n or names[i] not in STD_BONES)
           for i, n in enumerate(STD_BONES)):
        return None
    if "root" not in names:
        return None
    idx = {n: i for i, n in enumerate(names)}
    neww = {}
    for n in STD_BONES:
        if n in idx:
            neww[n] = world[idx[n]]
        else:
            pw = neww[STD_PARENT[n]]
            o = STD_OFFSET.get(n, (0.0, -0.03, 0.0))
            neww[n] = (pw[0] + o[0], pw[1] + o[1], pw[2] + o[2])
    # the text, nested as the retail 52 are
    kids = {}
    for n in STD_BONES:
        kids.setdefault(STD_PARENT[n], []).append(n)
    out = ["NumBones %d" % len(STD_BONES)]

    def emit(n, depth):
        p = STD_PARENT[n]
        pw = neww[p] if p else (0.0, 0.0, 0.0)
        w = neww[n]
        tab = "\t" * depth
        out.append("%sbone %s {" % (tab, n))
        out.append("%s\toffset %f %f %f" % (tab, w[0] - pw[0], w[1] - pw[1], w[2] - pw[2]))
        for k in kids.get(n, []):
            emit(k, depth + 1)
        out.append("%s}" % tab)
    emit("root", 0)
    new_text = "\n".join(out) + "\n"
    remap, delta = {}, {}
    for i, n in enumerate(names):
        a = i
        while bones[a][0] not in STD_PARENT and bones[a][1] >= 0:
            a = bones[a][1]
        target = bones[a][0] if bones[a][0] in STD_PARENT else "root"
        remap[i] = STD_BONES.index(target)
        tw = neww[target]
        delta[i] = (world[i][0] - tw[0], world[i][1] - tw[1], world[i][2] - tw[2])
    return new_text, remap, delta


def onto_skeleton(text, target):
    """As std_skeleton, onto a given skeleton (the retail one of the
    character copied, whose animations will play: Christina's are laid out
    for the retail ctn's 58 bones, hair included): its text, offsets
    changed to the preview's proportions where both have the bone, and the
    old bones' new ones and distances."""
    bones, world = skel_bones(text)
    tb, tw = skel_bones(target)
    names = [b[0] for b in bones]
    tnames = [b[0] for b in tb]
    idx = {n: i for i, n in enumerate(names)}
    if "root" not in idx or "root" not in tnames:
        return None
    neww = []
    for i, (n, p, o) in enumerate(tb):
        if n in idx:
            neww.append(world[idx[n]])
        else:
            pw = neww[p] if p >= 0 else (0.0, 0.0, 0.0)
            neww.append((pw[0] + o[0], pw[1] + o[1], pw[2] + o[2]))
    out = []
    stack = []
    k = -1
    for line in target.splitlines():
        if re.match(r"\s*bone\s+\S+", line):
            k += 1
            stack.append(k)
            out.append(line)
            continue
        if re.match(r"\s*\}", line) and stack:
            stack.pop()
            out.append(line)
            continue
        m = re.match(r"(\s*)offset\s", line)
        if m and stack:
            b = stack[-1]
            p = tb[b][1]
            pw = neww[p] if p >= 0 else (0.0, 0.0, 0.0)
            w = neww[b]
            out.append("%soffset %f %f %f" % (m.group(1), w[0] - pw[0], w[1] - pw[1], w[2] - pw[2]))
            continue
        out.append(line)
    tidx = {n: i for i, n in enumerate(tnames)}
    remap, delta = {}, {}
    for i, n in enumerate(names):
        a = i
        while bones[a][0] not in tidx and bones[a][1] >= 0:
            a = bones[a][1]
        t = tidx.get(bones[a][0], 0)
        remap[i] = t
        delta[i] = (world[i][0] - neww[t][0], world[i][1] - neww[t][1], world[i][2] - neww[t][2])
    return "\n".join(out) + "\n", remap, delta


def remap_bones(d, remap, delta):
    """A model in the model's space (to_model_space) moved onto other bones:
    each packet's matrices renumbered, a reskin's place (relative to its
    bone) moved by how far the new bone is from the old."""
    m = parse(d)
    out = bytearray(d)
    for p in m["packets"]:
        for k, b in enumerate(p["mtx"]):
            out[p["mtx_off"] + k] = remap.get(b, b)
        for (a, slot, wt, x, y, z), off in zip(p["rs"], p["rs_off"]):
            b = p["mtx"][slot] if slot < len(p["mtx"]) else None
            if b in delta:
                dx, dy, dz = delta[b]
                struct.pack_into("<3f", out, off + 8, x + dx, y + dy, z + dz)
    return bytes(out)


def is_212(d):
    return d[:13]==b'version: 2.12'

def model_height(d):
    m = parse(d)
    ys = [v[1] for v in m['v']]
    return (max(ys) - min(ys)) if ys else 0.0


def lod_to_model_space(d, skel_text, lod1_height):
    """A further LOD (lod_02, lod_03) moved into the model's space only when
    it is not there already. The preview keeps most models' vertices
    relative to their bones, but not every LOD: O'Grady's lod_02 and lod_03
    are in place (1.53 m tall as they are, his lod_01 1.52 once moved), and
    moved again by their bones they came out 2.89 m tall, arms like poles --
    the figure he became whenever he was far from the camera (user video).
    Whichever of the two comes nearer lod_01's height is kept."""
    moved = to_model_space(d, skel_text)
    if not lod1_height:
        return moved
    if abs(model_height(d) - lod1_height) < abs(model_height(moved) - lod1_height):
        return d
    return moved


def to_model_space(d, skel_text):
    """The model with each vertex moved by its bone's bind position."""
    m=parse(d)
    if m['end']!=len(d):
        raise ValueError('not a 2.12 model this parser knows')
    world=skel_world(skel_text)
    bone={}
    blended={}
    for p in m['packets']:
        for k,a in enumerate(p['adj']):
            if a[5]>=len(p['mtx']):
                # A slot past the packet's matrices: a vertex blended
                # between bones (its reskins: each bone's weight and the
                # vertex's place relative to it -- the preview's long-coat
                # Red, coc). In the model's space: the weighted sum.
                tw=sx=sy=sz=0.0
                for (ra,slot,wt,x,y,z) in p['rs']:
                    if ra!=k or slot>=len(p['mtx']) or p['mtx'][slot]>=len(world):
                        continue
                    w=world[p['mtx'][slot]]
                    tw+=wt; sx+=wt*(x+w[0]); sy+=wt*(y+w[1]); sz+=wt*(z+w[2])
                if tw>0 and a[0] not in blended:
                    blended[a[0]]=(sx/tw, sy/tw, sz/tw)
                continue
            b=p['mtx'][a[5]]
            if bone.setdefault(a[0], b)!=b:
                raise ValueError('vertex %d on two bones' % a[0])
    out=bytearray(d)
    for v,b in bone.items():
        if b>=len(world):
            raise ValueError('bone %d past the skeleton' % b)
        x,y,z=m['v'][v]; w=world[b]
        struct.pack_into('<3f', out, 58+12*v, x+w[0], y+w[1], z+w[2])
    for v,pos in blended.items():
        if v not in bone:
            struct.pack_into('<3f', out, 58+12*v, *pos)
    return bytes(out)

if __name__=='__main__' and len(sys.argv)==5 and sys.argv[1]=='convert':
    d=open(sys.argv[2],'rb').read()
    open(sys.argv[4],'wb').write(to_model_space(d, open(sys.argv[3]).read()))
elif __name__=='__main__':
    m=parse(open(sys.argv[1],'rb').read())
    print(m['counts'], 'end',m['end'],'len',m['len'], len(m['packets']))
    print(m['mats'][0])
    p=m['packets'][0]; print(len(p['adj']), p['prims'][:3], p['mtx'], p['rs'][:2])
