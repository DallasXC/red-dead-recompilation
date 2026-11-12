#!/usr/bin/env python3
"""
beta.py - list and extract the January 2002 preview's archive (PS2,
SCPS_666.66: SWAT.ZIP on the disc, which is not a zip).

Its format is the retail "Dave" archive's ancestor:

  0x000  "DAVE"  u32 count  u32 dir_bytes  u32 name_bytes
  0x800  count entries of { u32 name_offset, u32 data_offset,
                            u32 size, u32 packed_size }
  0x800 + dir_bytes   the names, plain zero-terminated text (the retail
                      archive packs them 6 bits a character)
  data   raw DEFLATE when packed_size < size, else stored.

Usage (SWAT.ZIP copied off the disc image, e.g. with 7-Zip):
  beta.py list    SWAT.ZIP
  beta.py extract SWAT.ZIP <out_dir> [pattern]
  beta.py levels  SWAT.ZIP          (each lvl_*.lvl and how much it holds)
"""
import fnmatch, os, re, struct, sys, zlib



def safe_dest(out, name):
    """Where an archive entry goes under out, or None for a name that would
    land outside it (.., an absolute path, a drive): a damaged or made-up
    archive cannot write elsewhere."""
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if not parts or any(p == ".." or ":" in p for p in parts) or name[:1] in ("/", "\\"):
        return None
    root = os.path.abspath(out)
    dst = os.path.abspath(os.path.join(root, *parts))
    if os.path.commonpath([root, dst]) != root:
        return None
    return dst

def entries(path):
    with open(path, 'rb') as f:
        head = f.read(16)
        if head[:4] != b'DAVE':
            sys.exit(f'{path}: not a DAVE archive')
        count, dir_bytes, name_bytes = struct.unpack_from('<III', head, 4)
        f.seek(0x800)
        d = f.read(dir_bytes + name_bytes)
    names = d[dir_bytes:]
    out = []
    for i in range(count):
        no, do, size, packed = struct.unpack_from('<IIII', d, i * 16)
        e = names.index(b'\0', no)
        out.append((names[no:e].decode('latin1'), do, size, packed))
    return out


def read(f, do, size, packed):
    f.seek(do)
    raw = f.read(packed)
    return raw if packed >= size else zlib.decompress(raw, -15)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    ents = entries(path)
    if cmd == 'list':
        for n, do, size, packed in ents:
            print(f'{size:10d} {packed:10d}  {n}')
    elif cmd == 'extract':
        out = sys.argv[3]
        pat = sys.argv[4].lower() if len(sys.argv) > 4 else '*'
        with open(path, 'rb') as f:
            for n, do, size, packed in ents:
                if not fnmatch.fnmatch(n.lower(), pat):
                    continue
                dst = safe_dest(out, n.replace('?', '%3f'))
                if dst is None:
                    print(f'skipped {n!r}: not a name inside the archive\'s folder', file=sys.stderr)
                    continue
                os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
                with open(dst, 'wb') as g:
                    g.write(read(f, do, size, packed))
    elif cmd == 'levels':
        low = {n.lower(): (do, size, packed) for n, do, size, packed in ents}
        with open(path, 'rb') as f:
            for n in sorted(x for x in low if x.endswith('.lvl')):
                text = read(f, *low[n]).decode('latin1')
                refs = re.findall(r'(\S+\.(?:bnds|znes|lnes|bxes|prps))', text)
                base = n[:-4] + '_'
                models = sum(1 for x in low if x.startswith(base) and x.endswith('.mod'))
                print(f'{n:28s} {len(refs):3d} parts, {models:4d} models')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
