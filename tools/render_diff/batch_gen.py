#!/usr/bin/env python3
"""
batch_gen.py -- Phase A of the full-library render-diff run.

Walks the whole PICO-8 cart library, generates a throwaway test wrapper for each
cart (shrinko8 decompress for .p8.png + harness inject), writes them to a temp
work dir + a manifest. Resumable: an already-generated wrapper is skipped.

The user's carts are NEVER modified (read-only). Wrappers go to Temp (throwaway,
no OneDrive sync thrash). Run from the MiSTerFrontier workspace root.

  python MiSTer_PICO-8/tools/render_diff/batch_gen.py
"""
import sys, os

ROOT = "PICO-8_Carts"
WORK = "C:/Users/miste/AppData/Local/Temp/rd_run"
WRAP = os.path.join(WORK, "wrap")
SHRINKO = "C:/Users/miste/AppData/Local/Temp/shrinko8"

sys.path.insert(0, os.path.join("MiSTer_PICO-8", "tools", "render_diff"))
import gen_wrapper as gw  # noqa: E402

def main():
    os.makedirs(WRAP, exist_ok=True)
    carts = []
    for dp, _, fns in os.walk(ROOT):
        for fn in fns:
            low = fn.lower()
            if low.endswith(".p8.png") or (low.endswith(".p8") and not low.endswith(".p8.png")):
                carts.append(os.path.join(dp, fn).replace("\\", "/"))
    carts.sort()
    man_path = os.path.join(WORK, "manifest.tsv")
    man = open(man_path, "w", encoding="utf-8", newline="\n")
    ok = fail = cached = 0
    for i, c in enumerate(carts):
        cid = "%05d" % i
        out = os.path.join(WRAP, cid + ".p8")
        if os.path.exists(out) and os.path.getsize(out) > 0:
            man.write(f"{cid}\t{c}\tcached\n"); cached += 1; ok += 1
        else:
            try:
                p8, tmp = gw.to_p8(c, SHRINKO)
                try:
                    gw.inject(p8, out, gw.DEFAULT_STOP)   # input ON -> reach gameplay
                finally:
                    if tmp and os.path.exists(tmp):
                        os.remove(tmp)
                man.write(f"{cid}\t{c}\tok\n"); ok += 1
            except Exception as e:
                man.write(f"{cid}\t{c}\tFAIL:{str(e)[:120].replace(chr(9),' ').replace(chr(10),' ')}\n")
                fail += 1
        if i % 100 == 0:
            print(f"{i}/{len(carts)} ok={ok} (cached={cached}) fail={fail}", flush=True)
    man.close()
    print(f"DONE gen: {ok} ok ({cached} cached), {fail} fail, total {len(carts)} -> {man_path}", flush=True)

if __name__ == "__main__":
    main()
