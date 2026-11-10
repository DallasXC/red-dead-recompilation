#!/usr/bin/env python3
"""
charpic.py - a Showdown character's pictures drawn from its model: the
cut-out on the line-up photo (ui_mphoto_<code>.xtex) and the portrait in
the player box (mp_<code>.xtex), for characters that come with no pictures
of their own (the 2002 preview's: beta_roster.py).

The model (a "version: 2.12" binary in the model's space, mod212.py) is
drawn from the front in its bind pose, textured, with a little light from
the upper left, then toned like the retail pictures: the cut-out's dark,
cool greys (the line-up tints them sepia), ink for the portrait.

  ui_mphoto_<code>  256 x 128, 8-bit palettised (format 0x0E: 14-byte
                    header, 256 RGBA entries, the indices), the figure
                    lying on its side, head to the left, feet at the right
                    edge, ~160 pixels tall, alpha around it -- as the
                    retail ones (ui_mphoto_srd)
  mp_<code>         128 x 128, A8R8G8B8 red first (format 0x12), head and
                    shoulders as ink: near-black, the darkness in alpha,
                    stored upside down -- as the retail ones (mp_srd)

  python tools\\mods\\charpic.py <model.xmod> <out.png> [texture dir]   (to look)
"""
import os, struct, sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mod212


def triangles(m):
    """(positions Nx3, uvs Nx2, material index) per triangle corner list."""
    P, UV, MAT = [], [], []
    for pk in m["packets"]:
        adj = pk["adj"]
        for t, idx in pk["prims"]:
            for i in range(len(idx) - 2):
                a, b, c = idx[i], idx[i + 1], idx[i + 2]
                if a == b or b == c or a == c:
                    continue
                for k in (a, b, c):
                    P.append(m["v"][adj[k][0]])
                    UV.append(m["t1"][adj[k][3]] if adj[k][3] < len(m["t1"]) else (0.0, 0.0))
                MAT.append(pk["mat"])
    return np.array(P, np.float64).reshape(-1, 3, 3), np.array(UV, np.float64).reshape(-1, 3, 2), MAT


def render(m, textures, w, h, region, facing=1):
    """Draw the model looking along -z*facing; region = (x0, x1, y0, y1) in
    model units mapped onto the image. Returns RGBA float array."""
    P, UV, MAT = triangles(m)
    x0, x1, y0, y1 = region
    img = np.zeros((h, w, 4), np.float64)
    zb = np.full((h, w), -1e9)
    light = np.array([-0.4, 0.5, 0.75]) * np.array([1, 1, facing])
    light /= np.linalg.norm(light)
    for tri, uv, mi in zip(P, UV, MAT):
        sx = (tri[:, 0] * facing - x0) / (x1 - x0) * w
        sy = (y1 - tri[:, 1]) / (y1 - y0) * h
        sz = tri[:, 2] * facing
        minx, maxx = int(max(0, np.floor(sx.min()))), int(min(w - 1, np.ceil(sx.max())))
        miny, maxy = int(max(0, np.floor(sy.min()))), int(min(h - 1, np.ceil(sy.max())))
        if minx > maxx or miny > maxy:
            continue
        d = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])
        if abs(d) < 1e-9:
            continue
        n = np.cross(tri[1] - tri[0], tri[2] - tri[0])
        ln = np.linalg.norm(n)
        shade = 0.55 + 0.45 * abs(np.dot(n / ln, light)) if ln > 0 else 0.8
        ys, xs = np.mgrid[miny:maxy + 1, minx:maxx + 1]
        px, py = xs + 0.5, ys + 0.5
        l1 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / d
        l2 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / d
        l3 = 1 - l1 - l2
        inside = (l1 >= -1e-6) & (l2 >= -1e-6) & (l3 >= -1e-6)
        if not inside.any():
            continue
        z = l1 * sz[0] + l2 * sz[1] + l3 * sz[2]
        sub = zb[miny:maxy + 1, minx:maxx + 1]
        ok = inside & (z > sub)
        if not ok.any():
            continue
        tex = textures.get(mi)
        if tex is not None:
            th, tw = tex.shape[:2]
            u = l1 * uv[0, 0] + l2 * uv[1, 0] + l3 * uv[2, 0]
            v = l1 * uv[0, 1] + l2 * uv[1, 1] + l3 * uv[2, 1]
            tx = np.clip((u % 1.0) * tw, 0, tw - 1).astype(int)
            ty = np.clip((v % 1.0) * th, 0, th - 1).astype(int)
            col = tex[ty, tx]
        else:
            col = np.full(ok.shape + (4,), 0.6)
            col[..., 3] = 1
        if col[..., 3].max() <= 0.01:      # an all-clear texture: draw it opaque
            col = col.copy()
            col[..., 3] = 1
        ok &= col[..., 3] > 0.3
        sub[ok] = z[ok]
        blk = img[miny:maxy + 1, minx:maxx + 1]
        blk[ok, :3] = col[ok, :3] * shade
        blk[ok, 3] = 1.0
    return img


def bounds(m):
    v = np.array(m["v"])
    return v.min(0), v.max(0)


def facing_of(m):
    """+1 when the model faces +z. Every one of the preview's models faces
    -z (checked by drawing all 55 from both sides). The guess by the feet
    used before (the toes reach further forward than the heels) took six
    for the other way -- "Big Oaf", "Bloody" Tom, Ydg, Dcg and two flat
    targets showed their backs on the line-up (user report)."""
    return -1


# The retail line-up cut-outs (ui_mphoto_*, 48 of them measured) are dark,
# cool greys -- the line-up's own film tint makes them sepia on the poster:
# luminance at the 5/25/50/75/95th percentiles 4, 19, 32, 53, 115 (of 255),
# each grey about 0.89 / 1.03 / 1.12 of its luminance in red / green / blue.
# Drawn bright and warm (as before) the preview's came out washed-out grey
# beside them (user report). A drawn picture's greys (the old toning, its
# percentiles 13, 48, 78, 123, 188 over the 53) are mapped onto the retail
# ones, then given their colour.
_TONE_FROM = np.array([0.0, 13.2, 48.0, 78.3, 123.1, 188.3, 255.0]) / 255.0
_TONE_TO = np.array([0.0, 4.0, 18.8, 32.4, 53.2, 115.0, 215.0]) / 255.0
RETAIL_TINT = (0.89, 1.03, 1.12)


def tone(img, tint):
    a = img[..., 3:4]
    g = (img[..., 0] * 0.3 + img[..., 1] * 0.59 + img[..., 2] * 0.11)
    g = np.clip((g - 0.05) * 1.25, 0, 1)
    g = np.interp(g, _TONE_FROM, _TONE_TO)[..., None]
    rgb = np.clip(g * np.array(tint), 0, 1)
    return np.concatenate([rgb, a], -1)


def model_height(m):
    lo, hi = bounds(m)
    return float(hi[1] - lo[1])


def cutout_xtex(m, textures, fig_px=160.0, feet_y=254.0):
    """The cut-out: the figure fig_px of the 256 pixels tall, its feet at
    feet_y (from the top, upright; the right edge once turned), centred
    across. The line-up copies it 1:1 onto the 1024 x 256 photo, so where
    the figure stands in its picture is where it stands on the photo: the
    retail ones stand in three rows that way (feet near 190, 245 and 253)."""
    lo, hi = bounds(m)
    f = facing_of(m)
    height = hi[1] - lo[1]
    H, W = 256, 128
    scale = float(fig_px) / height
    cx = (lo[0] + hi[0]) / 2 * f
    span_y = H / scale
    span_x = W / scale
    below = (H - float(feet_y)) / scale
    region = (cx - span_x / 2, cx + span_x / 2, lo[1] - below, lo[1] - below + span_y)
    img = render(m, textures, W, H, region, f)
    img = tone(img, RETAIL_TINT)
    im = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8), "RGBA")
    im = im.rotate(90, expand=True)         # head to the left: 256 x 128
    rgb = im.convert("RGB").quantize(colors=255, method=Image.MEDIANCUT)
    pal = rgb.getpalette()[:255 * 3]
    idx = np.array(rgb, np.uint8)
    alpha = np.array(im)[..., 3]
    idx = np.where(alpha > 127, idx + 1, 0).astype(np.uint8)
    out = bytearray(struct.pack("<7H", 256, 128, 14, 1, 1, 1, 1))
    out += bytes((0, 0, 0, 0))
    for i in range(255):
        r, g, b = pal[3 * i:3 * i + 3] if 3 * i + 2 < len(pal) else (0, 0, 0)
        out += bytes((r, g, b, 255))
    out += idx.tobytes()
    return bytes(out), im


def portrait_xtex(m, textures):
    """The portrait as the retail ones are made: ink on the box's paper --
    near-black with the darkness in alpha (light skin faint, a hat brim
    solid), nothing around the figure, stored upside down (mp_srd)."""
    lo, hi = bounds(m)
    f = facing_of(m)
    height = hi[1] - lo[1]
    cx = (lo[0] + hi[0]) / 2 * f
    side = height * 0.30
    region = (cx - side / 2, cx + side / 2, hi[1] - side * 0.95, hi[1] + side * 0.05)
    img = render(m, textures, 128, 128, region, f)
    g = img[..., 0] * 0.3 + img[..., 1] * 0.59 + img[..., 2] * 0.11
    ink = np.clip(0.25 + 0.85 * (1.0 - g), 0, 1) * img[..., 3]
    shown = Image.fromarray((255 - ink * 255).astype(np.uint8), "L").convert("RGB")
    a8 = (ink * 255).astype(np.uint8)[::-1]
    out = bytearray(struct.pack("<7H", 128, 128, 0x12, 1, 1, 0, 0))
    out += np.stack([np.full_like(a8, 20)] * 3 + [a8], -1).tobytes()
    return bytes(out), shown


def head_place(skel_text, cr_text=None):
    """Where the game puts a character's separate face (<render>_face.xmod,
    its .cr's "Face 1"): its vertices are around the head bone, so the
    head's bind place, plus the .cr's FaceOffset."""
    import re
    bones, world = mod212.skel_bones(skel_text)
    at = next((world[i] for i, b in enumerate(bones) if b[0].lower() == "head"), None)
    if at is None:
        return None
    if cr_text:
        if isinstance(cr_text, bytes):
            cr_text = cr_text.decode("latin-1")
        mo = re.search(r"FaceOffset\s+([-0-9.eE]+)\s+([-0-9.eE]+)\s+([-0-9.eE]+)", cr_text)
        if mo:
            at = tuple(a + float(b) for a, b in zip(at, mo.groups()))
    return at


def with_face(m, face, at):
    """The model and its separate face, one model to draw: the face's
    vertices moved to the head (head_place). Without it the pictures of a
    character whose face is its own model had none (user report: the
    story's townsfolk, faceless on the line-up and in the player box)."""
    if face is None or at is None:
        return m
    nv, nt, nm = len(m["v"]), len(m["t1"]), len(m["mats"])
    out = dict(m)
    out["v"] = list(m["v"]) + [(x + at[0], y + at[1], z + at[2]) for x, y, z in face["v"]]
    out["t1"] = list(m["t1"]) + list(face["t1"])
    out["mats"] = list(m["mats"]) + list(face["mats"])
    pks = list(m["packets"])
    for pk in face["packets"]:
        q = dict(pk)
        q["adj"] = [(a[0] + nv, a[1], a[2], a[3] + nt) + tuple(a[4:]) for a in pk["adj"]]
        q["mat"] = pk["mat"] + nm
        pks.append(q)
    out["packets"] = pks
    return out


def textures_for(m, lookup):
    """{material index: float RGBA array} -- lookup(name) -> PIL image or None."""
    out = {}
    for i, mt in enumerate(m["mats"]):
        if mt["tex"]:
            im = lookup(mt["tex"][0])
            if im is not None:
                out[i] = np.asarray(im.convert("RGBA"), np.float64) / 255.0
    return out


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        sys.exit(1)
    m = mod212.parse(open(a[0], "rb").read())
    tdir = a[2] if len(a) > 2 else None

    def lookup(name):
        if not tdir:
            return None
        for f in os.listdir(tdir):
            if f.lower() == name.lower() + ".png":
                return Image.open(os.path.join(tdir, f))
        return None
    tex = textures_for(m, lookup)
    _, cut = cutout_xtex(m, tex)
    _, por = portrait_xtex(m, tex)
    c = Image.new("RGBA", (256 + 128, 128), (0, 100, 0, 255))
    c.alpha_composite(cut, (0, 0))
    c.paste(por, (256, 0))
    c.save(a[1])


if __name__ == "__main__":
    main()
