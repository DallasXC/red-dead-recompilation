#!/usr/bin/env python3
"""
upscale_pack.py - make an upscaled texture pack from your own texture dump.

The game's textures are not shipped with this project, so an HD pack is
made on your PC from the textures your game dumps:

  1. Graphics page: switch on "Dump textures", restart, and play through the
     places you want sharper (every texture the game loads is saved to
     texture_dump\\ beside settings.ini as <hash>.tga). Switch it off again.
  2. Run this tool:
        python tools\\mods\\upscale_pack.py "<play folder>\\texture_dump" ^
               "<play folder>\\Mods\\textures\\HD pack" --scale 4
  3. The pack is on the launcher's Mods page (Texture packs), on by default.

How the pictures are enlarged:
  --esrgan <realesrgan-ncnn-vulkan.exe>   an AI upscaler, much sharper (free:
        https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan -- unpack it and
        give the path of the .exe; --model picks one of its models, default
        realesrgan-x4plus; the GPU does the work)
  otherwise                                  a clean Lanczos enlargement with
        a light sharpen (Pillow), on the CPU: softer, but needs nothing else.

The colour goes through the upscaler; the alpha (transparency) is enlarged
smoothly on its own, so cut-out edges (leaves, fences, hair) stay clean.
Textures smaller than --min (default 16 pixels on their longer side) and the
game's font pages (they are drawn sharp already: Graphics > Sharp game text)
are left as they are. Already made pictures are skipped, so it can be
stopped and run again; --redo makes them all again.

Needs Python 3 and Pillow (pip install pillow).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

try:
    from PIL import Image, ImageFilter
except ImportError:
    sys.exit("Pillow is needed: pip install pillow")


def is_font_page(img):
    """The font pages: palette-like, alpha-only glyph sheets (white or one
    colour, the letters in the alpha) -- best left to the game's own sharp
    text."""
    if img.mode != "RGBA":
        return False
    w, h = img.size
    if w < 64 or h < 64:
        return False
    small = img.resize((32, 32))
    px = list(small.getdata())
    rgb = {(r // 16, g // 16, b // 16) for r, g, b, a in px}
    alphas = {a for r, g, b, a in px}
    return len(rgb) <= 2 and len(alphas) > 4


def lanczos(img, scale):
    w, h = img.size
    big = img.resize((w * scale, h * scale), Image.LANCZOS)
    return big.filter(ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))


def esrgan(exe, model, rgb, scale, work):
    src = os.path.join(work, "in.png")
    dst = os.path.join(work, "out.png")
    rgb.save(src)
    cmd = [exe, "-i", src, "-o", dst, "-n", model, "-s", str(scale)]
    r = subprocess.run(cmd, cwd=os.path.dirname(exe) or None, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0 or not os.path.exists(dst):
        raise RuntimeError(r.stderr.decode(errors="replace")[-400:])
    out = Image.open(dst).convert("RGB")
    want = (rgb.size[0] * scale, rgb.size[1] * scale)
    if out.size != want:
        out = out.resize(want, Image.LANCZOS)
    os.remove(dst)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="the texture_dump folder")
    ap.add_argument("pack", help="the pack folder to write (in Mods\\textures)")
    ap.add_argument("--scale", type=int, default=4, choices=(2, 3, 4))
    ap.add_argument("--esrgan", help="path of realesrgan-ncnn-vulkan(.exe)")
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--min", type=int, default=16, help="smaller textures are left out")
    ap.add_argument("--fonts", action="store_true", help="upscale the font pages too")
    ap.add_argument("--redo", action="store_true", help="make every picture again")
    a = ap.parse_args()

    if not os.path.isdir(a.dump):
        sys.exit("no folder %s" % a.dump)
    if a.esrgan and not os.path.isfile(a.esrgan):
        sys.exit("no upscaler at %s" % a.esrgan)
    os.makedirs(a.pack, exist_ok=True)
    names = sorted(n for n in os.listdir(a.dump) if n.lower().endswith(".tga"))
    work = tempfile.mkdtemp(prefix="rdr_upscale_")
    done = skipped = failed = 0
    try:
        for i, n in enumerate(names):
            out = os.path.join(a.pack, n)
            if os.path.exists(out) and not a.redo:
                skipped += 1
                continue
            try:
                img = Image.open(os.path.join(a.dump, n))
                img.load()
            except Exception as e:
                print("  %s: cannot read (%s)" % (n, e))
                failed += 1
                continue
            if max(img.size) < a.min or (not a.fonts and is_font_page(img.convert("RGBA"))):
                skipped += 1
                continue
            img = img.convert("RGBA")
            rgb = img.convert("RGB")
            alpha = img.getchannel("A")
            try:
                if a.esrgan:
                    big = esrgan(a.esrgan, a.model, rgb, a.scale, work)
                else:
                    big = lanczos(rgb, a.scale)
            except Exception as e:
                print("  %s: the upscaler failed (%s); Lanczos instead" % (n, e))
                big = lanczos(rgb, a.scale)
            big_a = alpha.resize(big.size, Image.LANCZOS)
            res = big.convert("RGBA")
            res.putalpha(big_a)
            res.save(out)
            done += 1
            if done % 25 == 0:
                print("%d of %d" % (i + 1, len(names)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("done: %d made, %d left as they are or already there, %d not readable -> %s"
          % (done, skipped, failed, a.pack))


if __name__ == "__main__":
    main()
