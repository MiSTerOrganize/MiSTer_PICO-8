import sys, collections
def load(p):
    rows={}
    for ln in open(p,encoding="utf-8",errors="replace"):
        ln=ln.strip()
        if not ln.startswith("FBDUMP"): continue
        _,r,hexs=ln.split(None,2)
        rows[int(r)]=hexs.strip()
    return rows
z=load(sys.argv[1]); o=load(sys.argv[2])
# decode 4bpp: byte i (hexpair) -> pixel 2i = low nibble, 2i+1 = high nibble
def px(rows):
    img=[[0]*128 for _ in range(128)]
    for r in range(128):
        h=rows.get(r,"")
        for c in range(64):
            if 2*c+1>=len(h): break
            b=int(h[2*c:2*c+2],16)
            img[r][2*c]=b&0xf
            img[r][2*c+1]=b>>4
    return img
zi=px(z); oi=px(o)
diffs=[]; rowcount=collections.Counter(); pairs=collections.Counter()
for y in range(128):
    for x in range(128):
        if zi[y][x]!=oi[y][x]:
            diffs.append((x,y,zi[y][x],oi[y][x])); rowcount[y]+=1; pairs[(zi[y][x],oi[y][x])]+=1
print(f"total differing pixels: {len(diffs)} / 16384")
if diffs:
    xs=[d[0] for d in diffs]; ys=[d[1] for d in diffs]
    print(f"bounding box: x[{min(xs)}..{max(xs)}] y[{min(ys)}..{max(ys)}]")
    print("top (z8->official) index swaps:", pairs.most_common(8))
    print("sample diffs (x,y,z8,off):", diffs[:12])
    # CLUSTER ANALYSIS -- the trustworthiness verdict. Scattered single pixels
    # (starfields/particles/frame-pacing) are FALSE POSITIVES; a real render bug is
    # CLUSTERED (a broken sprite / wrong colours over a contiguous region).
    dset=set((x,y) for x,y,_,_ in diffs)
    seen=set(); largest=0
    for p in dset:
        if p in seen: continue
        st=[p]; seen.add(p); sz=0
        while st:
            cx,cy=st.pop(); sz+=1
            for dx,dy in ((1,0),(-1,0),(0,1),(0,-1)):
                q=(cx+dx,cy+dy)
                if q in dset and q not in seen: seen.add(q); st.append(q)
        if sz>largest: largest=sz
    cell=collections.Counter((x//16,y//16) for x,y in dset)
    maxcell=max(cell.values()) if cell else 0
    total=len(diffs)
    # background-swap ratio: in a moving particle field (starfield/snow/sparks) almost
    # every diff pixel has bg (0/1) on ONE side -- a star that moved. A real render bug
    # (wrong palette / broken sprite) swaps real colour for real colour. This separates
    # dense-starfield FPs (which trip raw cluster size) from genuine clustered bugs.
    bgswap=sum(1 for _,_,a,b in diffs if a in (0,1) or b in (0,1))
    bgr=bgswap/total
    # magnitude is a HINT for sort order, NOT a verdict. Validated 2026-06-18 that a
    # dense starfield (Wander) reaches largest=56/cell=49 yet is a pure FP -> the
    # triptych PNG is the authority. High solid-colour clustering with LOW bg-swap is
    # the only auto-flag worth surfacing first.
    hint = "LIKELY-REAL" if (largest>=24 and bgr<0.5) or total>=1500 else \
           ("PARTICLE-FP?" if bgr>=0.85 else "REVIEW-IMAGE")
    print(f"cluster: total={total} largest_cc={largest} densest_16x16={maxcell} bg_swap_ratio={bgr:.2f}")
    print(f"HINT: {hint}  (sort-key only -- the diff PNG is the authority; eyeball it)")
# render PNG (PICO-8 palette) via stdlib zlib
PAL=[(0,0,0),(29,43,83),(126,37,83),(0,135,81),(171,82,54),(95,87,79),(194,195,199),(255,241,232),(255,0,77),(255,163,0),(255,236,39),(0,228,54),(41,173,255),(131,118,156),(255,119,168),(255,204,170)]
import zlib,struct
def png(path,img,scale=3):
    W=H=128*scale
    raw=bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            r=PAL[img[y//scale][x//scale]]
            raw+=bytes(r)
    def chunk(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
    sig=b"\x89PNG\r\n\x1a\n"
    ihdr=struct.pack(">IIBBBBB",W,H,8,2,0,0,0)
    open(path,"wb").write(sig+chunk(b"IHDR",ihdr)+chunk(b"IDAT",zlib.compress(bytes(raw),9))+chunk(b"IEND",b""))
png(sys.argv[3],zi); png(sys.argv[4],oi)
# diff overlay: magenta where differ, else official dimmed
dimg=[[oi[y][x] for x in range(128)] for y in range(128)]
for x,y,_,_ in diffs: dimg[y][x]=14
png(sys.argv[5],dimg)
print("wrote z8/official/diff PNGs")
