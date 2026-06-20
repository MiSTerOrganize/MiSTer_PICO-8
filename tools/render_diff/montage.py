#!/usr/bin/env python3
"""
montage.py -- eyeball-pass accelerator: tile z8|official thumbnails for fast scan.

Reads the ranked candidate order (ranked.txt) + the triptych PNGs (<id>_z8.png,
<id>_off.png written by rank_triptychs/fbdiff), downsamples each to a thumbnail and
lays them out z8|official side-by-side in a grid, N carts per montage, in rank order.
One montage image scans ~24 carts at once: a REAL bug (wrong colours / missing region)
pops as a colour/structure mismatch between the side-by-side pair; an animation FP shows
the same content shifted. Flag suspicious pairs -> confirm at full res.

  python montage.py [per_montage=24] [thumb=46]
"""
import os, struct, zlib, sys
WORK = "C:/Users/miste/AppData/Local/Temp/rd_run"
TRI = os.path.join(WORK, "triptych")
OUTDIR = os.path.join(WORK, "montage")

def decode_rgb(p):
    d = open(p, "rb").read(); i = 8; w = h = 0; idat = b""
    while i < len(d):
        ln = struct.unpack(">I", d[i:i+4])[0]; typ = d[i+4:i+8]; dat = d[i+8:i+8+ln]; i += 12+ln
        if typ == b"IHDR": w, h = struct.unpack(">II", dat[:8])
        elif typ == b"IDAT": idat += dat
        elif typ == b"IEND": break
    raw = zlib.decompress(idat); stride = w*3
    rows = []
    pos = 0
    for y in range(h):
        pos += 1  # filter byte (fbdiff writes 0 = none)
        rows.append(raw[pos:pos+stride]); pos += stride
    return w, h, rows

def thumb(rows, w, h, t):
    out = []
    for ty in range(t):
        sy = ty*h//t
        row = rows[sy]
        line = bytearray()
        for tx in range(t):
            sx = tx*w//t
            line += row[sx*3:sx*3+3]
        out.append(bytes(line))
    return out  # t rows of t*3 bytes

def png(path, w, h, rows):
    raw = bytearray()
    for r in rows:
        raw.append(0); raw += r
    def ch(tp, dt): return struct.pack(">I", len(dt))+tp+dt+struct.pack(">I", zlib.crc32(tp+dt)&0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
        ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
        ch(b"IDAT", zlib.compress(bytes(raw), 9)) + ch(b"IEND", b""))

def main():
    per = int(sys.argv[1]) if len(sys.argv) > 1 else 24
    t = int(sys.argv[2]) if len(sys.argv) > 2 else 46
    os.makedirs(OUTDIR, exist_ok=True)
    ids = []
    for ln in open(os.path.join(WORK, "ranked.txt"), encoding="utf-8"):
        p = ln.split()
        if len(p) >= 5 and p[4].isdigit() and len(p[4]) == 5:
            ids.append(p[4])
    cols = 6              # 6 pairs across
    gap = 3
    pairw = t*2 + gap
    cellw = pairw + gap
    rows_per = (per + cols - 1)//cols
    cellh = t + 12
    mn = 0
    for base in range(0, len(ids), per):
        chunk = ids[base:base+per]
        W = cols*cellw + gap; H = rows_per*cellh + gap
        canvas = [bytearray(b"\x20\x20\x20"*W) for _ in range(H)]  # grey bg
        for k, cid in enumerate(chunk):
            r = k//cols; c = k % cols
            x0 = gap + c*cellw; y0 = gap + r*cellh
            for tag, dx in (("_z8.png", 0), ("_off.png", t)):
                fp = os.path.join(TRI, cid+tag)
                if not os.path.exists(fp): continue
                try:
                    w, h, rws = decode_rgb(fp); th = thumb(rws, w, h, t)
                except Exception:
                    continue
                for yy in range(t):
                    row = canvas[y0+yy]; src = th[yy]
                    px = (x0+dx)*3
                    row[px:px+t*3] = src
        png(os.path.join(OUTDIR, f"montage_{mn:02d}.png"), W, H, [bytes(r) for r in canvas])
        # print id map for this montage (rank order, row-major)
        print(f"montage_{mn:02d}.png  (rank {base+1}-{base+len(chunk)}):")
        for r in range((len(chunk)+cols-1)//cols):
            print("  " + "  ".join(chunk[r*cols:(r+1)*cols]))
        mn += 1
    print(f"\n{mn} montages -> {OUTDIR}  (each cell = z8 | official, rank order L->R top->bottom)")

if __name__ == "__main__":
    main()
