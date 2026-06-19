#!/usr/bin/env python3
"""
batch_seq_gen.py -- Tier-2 step 1: gen SEQ wrappers for the candidate ids.

Reads a candidate id list (one 5-digit id per line, e.g. from compare_render.py's
RENDER-DIVERGE/AUDIO-DIVERGE/NO-GAMEPLAY rows) + the manifest, and generates a
--seq wrapper (hash every frame 1..SEQ, no input) for each into rd_run/cand_wrap/.

  python batch_seq_gen.py <candidates.txt> [seqN]
"""
import sys, os
WORK = "C:/Users/miste/AppData/Local/Temp/rd_run"
CAND = os.path.join(WORK, "cand_wrap")
SHRINKO = "C:/Users/miste/AppData/Local/Temp/shrinko8"
sys.path.insert(0, os.path.join("MiSTer_PICO-8", "tools", "render_diff"))
import gen_wrapper as gw  # noqa: E402

def main():
    cand_file = sys.argv[1]
    seqn = int(sys.argv[2]) if len(sys.argv) > 2 else 90
    os.makedirs(CAND, exist_ok=True)
    man = {}
    for ln in open(os.path.join(WORK, "manifest.tsv"), encoding="utf-8"):
        p = ln.rstrip("\n").split("\t")
        if len(p) >= 2:
            man[p[0]] = p[1]
    ids = [l.strip() for l in open(cand_file, encoding="utf-8") if l.strip()]
    ok = fail = 0
    for cid in ids:
        c = man.get(cid)
        out = os.path.join(CAND, cid + ".p8")
        if not c:
            print(f"{cid}: not in manifest", flush=True); fail += 1; continue
        if os.path.exists(out) and os.path.getsize(out) > 0:
            ok += 1; continue
        try:
            p8, tmp = gw.to_p8(c, SHRINKO)
            try:
                gw.inject(p8, out, gw.DEFAULT_STOP, seq=seqn)
            finally:
                if tmp and os.path.exists(tmp):
                    os.remove(tmp)
            ok += 1
        except Exception as e:
            print(f"{cid}: FAIL {str(e)[:100]}", flush=True); fail += 1
    print(f"DONE seq-gen: {ok} ok, {fail} fail -> {CAND}", flush=True)

if __name__ == "__main__":
    main()
