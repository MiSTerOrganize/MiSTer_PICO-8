#!/usr/bin/env python3
"""
batch_win_gen.py -- window-dump wrappers for the pixel-window classifier.
Gen a --dumpwin 30 90 (no-input, frame-tagged 128x128 dump per frame) wrapper for each
candidate id into rd_run/win_wrap/. Run both engines (batch_win_z8.sh / _official.ps1) ->
pixel_window_classify.py.
  python batch_win_gen.py <candidates.txt> [s=30] [e=90]
"""
import sys, os
WORK = "C:/Users/miste/AppData/Local/Temp/rd_run"
OUT = os.path.join(WORK, "win_wrap")
SHRINKO = "C:/Users/miste/AppData/Local/Temp/shrinko8"
sys.path.insert(0, os.path.join("MiSTer_PICO-8", "tools", "render_diff"))
import gen_wrapper as gw  # noqa: E402

def main():
    cand = sys.argv[1]
    s = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    e = int(sys.argv[3]) if len(sys.argv) > 3 else 90
    os.makedirs(OUT, exist_ok=True)
    man = {}
    for ln in open(os.path.join(WORK, "manifest.tsv"), encoding="utf-8"):
        p = ln.rstrip("\n").split("\t")
        if len(p) >= 2:
            man[p[0]] = p[1]
    ok = fail = 0
    for cid in [l.strip() for l in open(cand, encoding="utf-8") if l.strip()]:
        c = man.get(cid); out = os.path.join(OUT, cid + ".p8")
        if not c:
            fail += 1; continue
        if os.path.exists(out) and os.path.getsize(out) > 0:
            ok += 1; continue
        try:
            p8, tmp = gw.to_p8(c, SHRINKO)
            try:
                gw.inject(p8, out, gw.DEFAULT_STOP, dwin=(s, e))
            finally:
                if tmp and os.path.exists(tmp):
                    os.remove(tmp)
            ok += 1
        except Exception as ex:
            print(f"{cid}: FAIL {str(ex)[:80]}", flush=True); fail += 1
        if ok % 200 == 0:
            print(f"{ok} gen...", flush=True)
    print(f"DONE win-gen ({s}-{e}): {ok} ok, {fail} fail -> {OUT}", flush=True)

if __name__ == "__main__":
    main()
