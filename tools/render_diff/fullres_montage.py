#!/usr/bin/env python3
"""
fullres_montage.py -- FULL-RESOLUTION eyeball sheets from window dumps (no downsampling).

The 46px montage was lossy and missed bugs. This renders each candidate's anchor frame
at native 128x128 for BOTH engines, side by side (z8 | official), tiled N per sheet, in
ranked order. Reads win_z8/<id>.txt + win_off/<id>.txt (frame-tagged FBDUMP). A real bug
shows a colour/structure mismatch between the side-by-side pair; an animation FP shows the
same content shifted. Every pixel is real -- nothing hidden by scaling.

  python fullres_montage.py <id-list.txt> [anchor=60] [per_row=5] [rows=5]
"""
import sys, os, struct, zlib
WORK = "C:/Users/miste/AppData/Local/Temp/rd_run"
ZD = os.path.join(WORK, "win_z8"); OD = os.path.join(WORK, "win_off")
OUTDIR = os.path.join(WORK, "fr_montage")
PAL = [(0,0,0),(29,43,83),(126,37,83),(0,135,81),(171,82,54),(95,87,79),(194,195,199),(255,241,232),
       (255,0,77),(255,163,0),(255,236,39),(0,228,54),(41,173,255),(131,118,156),(255,119,168),(255,204,170)]

def frame(path, anchor):
    rows = {}
    if not os.path.exists(path): return None
    for ln in open(path, encoding="utf-8", errors="replace"):
        ln = ln.strip()
        if not ln.startswith("FBDUMP"): continue
        p = ln.split(None, 3)
        if len(p) < 4: continue
        if int(p[1]) == anchor: rows[int(p[2])] = p[3]
    if len(rows) < 128:
        # fall back to any present frame near anchor
        return None
    return [rows[i] for i in range(128)]

def png(path, w, h, rgb_rows):
    raw = bytearray()
    for r in rgb_rows:
        raw.append(0); raw += r
    def ch(t, d): return struct.pack(">I", len(d))+t+d+struct.pack(">I", zlib.crc32(t+d)&0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
        ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
        ch(b"IDAT", zlib.compress(bytes(raw), 9)) + ch(b"IEND", b""))

def main():
    ids = [l.split()[0] if l.split() else "" for l in open(sys.argv[1], encoding="utf-8") if l.strip()]
    ids = [i for i in ids if len(i) == 5 and i.isdigit()]
    anchor = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    per_row = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    rows_per = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    os.makedirs(OUTDIR, exist_ok=True)
    per = per_row * rows_per
    gap = 4; cell_w = 128*2 + gap          # z8 | official
    lab = 10
    cellw = cell_w + gap; cellh = 128 + lab + gap
    W = per_row*cellw + gap
    sheet = 0
    for base in range(0, len(ids), per):
        chunk = ids[base:base+per]
        H = rows_per*cellh + gap
        canvas = [bytearray(b"\x28\x28\x28"*W) for _ in range(H)]
        for k, cid in enumerate(chunk):
            zf = frame(os.path.join(ZD, cid+".txt"), anchor)
            of = frame(os.path.join(OD, cid+".txt"), anchor)
            r = k//per_row; c = k % per_row
            x0 = gap + c*cellw; y0 = gap + lab + r*cellh
            for src, dx in ((zf, 0), (of, 128+gap//2)):
                if src is None: continue
                for yy in range(128):
                    line = canvas[y0+yy]; px = (x0+dx)*3
                    rowhex = src[yy]
                    seg = bytearray()
                    for ch_ in rowhex[:128]:
                        seg += bytes(PAL[int(ch_, 16)])
                    line[px:px+128*3] = seg
        png(os.path.join(OUTDIR, f"fr_{sheet:02d}.png"), W, H, [bytes(r) for r in canvas])
        print(f"fr_{sheet:02d}.png (rank {base+1}-{base+len(chunk)}):")
        for r in range((len(chunk)+per_row-1)//per_row):
            print("  " + "  ".join(chunk[r*per_row:(r+1)*per_row]))
        sheet += 1
    print(f"\n{sheet} full-res sheets -> {OUTDIR} (each cell: z8 LEFT | official RIGHT, native 128px)")

if __name__ == "__main__":
    main()
