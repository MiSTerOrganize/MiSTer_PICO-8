#!/usr/bin/env python3
"""
batch_dump_gen.py -- eyeball-pass step 1: gen no-input FBDUMP wrappers for candidates.

For each candidate id, emit a --noinput --dumpframe F wrapper (dumps the full 128x128
framebuffer at frame F as hex, then stop) into rd_run/dump_wrap/. Run on both engines
(batch_dump_z8.sh / batch_dump_official.ps1) -> fbdiff triptychs -> eyeball.

  python batch_dump_gen.py <candidates.txt> [frame=60]
"""
import sys, os
WORK = "C:/Users/miste/AppData/Local/Temp/rd_run"
OUT = os.path.join(WORK, "dump_wrap")
SHRINKO = "C:/Users/miste/AppData/Local/Temp/shrinko8"
sys.path.insert(0, os.path.join("MiSTer_PICO-8", "tools", "render_diff"))
import gen_wrapper as gw  # noqa: E402

def main():
    cand = sys.argv[1]
    frame = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    os.makedirs(OUT, exist_ok=True)
    man = {}
    for ln in open(os.path.join(WORK, "manifest.tsv"), encoding="utf-8"):
        p = ln.rstrip("\n").split("\t")
        if len(p) >= 2:
            man[p[0]] = p[1]
    ok = fail = 0
    for cid in [l.strip() for l in open(cand, encoding="utf-8") if l.strip()]:
        c = man.get(cid)
        out = os.path.join(OUT, cid + ".p8")
        if not c:
            fail += 1; continue
        if os.path.exists(out) and os.path.getsize(out) > 0:
            ok += 1; continue
        try:
            p8, tmp = gw.to_p8(c, SHRINKO)
            try:
                gw.inject(p8, out, gw.DEFAULT_STOP, noinput=True, dumpframe=frame)
            finally:
                if tmp and os.path.exists(tmp):
                    os.remove(tmp)
            ok += 1
        except Exception as e:
            print(f"{cid}: FAIL {str(e)[:80]}", flush=True); fail += 1
    print(f"DONE dump-gen (frame {frame}): {ok} ok, {fail} fail -> {OUT}", flush=True)

if __name__ == "__main__":
    main()
