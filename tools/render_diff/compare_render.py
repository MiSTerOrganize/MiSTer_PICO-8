#!/usr/bin/env python3
"""
compare_render.py -- render-diff triage (Milestone 3).
Joins the z8headless and official-PICO-8 result files (FBHASH/AUDHASH per cart,
'## <name>' headers) and classifies each cart:
  MATCH            render + audio identical at every common checkpoint
  RENDER-DIVERGE   a checkpoint FBHASH differs (= candidate zepto8 render bug)
  AUDIO-DIVERGE    a checkpoint AUDHASH differs (cart triggered different sfx)
  NO-GAMEPLAY?     <=2 distinct FB hashes (likely stuck at a static title)
  NO-CHECKPOINTS   one side produced nothing (errored / didn't run)
Usage: compare_render.py z8_results.txt official_results.txt [manifest.txt]
"""
import sys, re

def parse(path):
    carts, cur = {}, None
    for ln in open(path, encoding="utf-8", errors="replace"):
        ln = ln.rstrip("\n")
        if ln.startswith("## "):
            cur = ln[3:].strip(); carts[cur] = {"FB": {}, "AUD": {}, "oversize": False}
        elif cur and ln.strip() == "OVERSIZE":
            carts[cur]["oversize"] = True
        elif cur and ln.startswith(("FBHASH", "AUDHASH")):
            m = re.match(r"(FBHASH|AUDHASH) f(\d+)=(.*)", ln)
            if m:
                kind = "FB" if m.group(1) == "FBHASH" else "AUD"
                carts[cur][kind][int(m.group(2))] = m.group(3)
    return carts

def main():
    z8 = parse(sys.argv[1]); off = parse(sys.argv[2])
    names = {}
    if len(sys.argv) > 3:
        for ln in open(sys.argv[3], encoding="utf-8", errors="replace"):
            if "|" in ln: k, v = ln.strip().split("|", 1); names[k] = v
    allc = sorted(set(z8) | set(off))
    nmatch = ndiv = naud = nstuck = nerr = noversize = 0
    print(f"=== render-diff triage: {len(allc)} carts ===")
    for c in allc:
        zb = z8.get(c, {"FB": {}, "AUD": {}, "oversize": False})
        ob = off.get(c, {"FB": {}, "AUD": {}, "oversize": False})
        label = names.get(c, c)
        if ob.get("oversize"):
            noversize += 1; print(f"WRAP-OVERSIZE   {c}  (cart fills official's 8192-token budget; can't auto-wrap)  {label}"); continue
        if not zb["FB"] or not ob["FB"]:
            nerr += 1; print(f"NO-CHECKPOINTS  {c}  (z8={len(zb['FB'])} off={len(ob['FB'])})  {label}"); continue
        common = sorted(set(zb["FB"]) & set(ob["FB"]))
        fbdiff = [f for f in common if zb["FB"][f] != ob["FB"][f]]
        auddiff = [f for f in sorted(set(zb["AUD"]) & set(ob["AUD"])) if zb["AUD"][f] != ob["AUD"][f]]
        distinct = len(set(ob["FB"].values()))
        if fbdiff:
            ndiv += 1
            f0 = fbdiff[0]
            print(f"RENDER-DIVERGE  {c}  @f{f0} z8={zb['FB'][f0]} off={ob['FB'][f0]}  ({len(fbdiff)}/{len(common)} cks)  {label}")
        elif auddiff:
            naud += 1
            print(f"AUDIO-DIVERGE   {c}  @f{auddiff[0]} ({len(auddiff)} cks)  {label}")
        elif distinct <= 2:
            nstuck += 1
            print(f"NO-GAMEPLAY?    {c}  (only {distinct} distinct frame(s) -- static/title?)  {label}")
        else:
            nmatch += 1
            print(f"MATCH           {c}  ({len(common)} cks, {distinct} distinct frames)  {label}")
    print(f"\n=== {nmatch} MATCH | {ndiv} RENDER-DIVERGE | {naud} AUDIO-DIVERGE | {nstuck} NO-GAMEPLAY? | {noversize} WRAP-OVERSIZE | {nerr} NO-CHECKPOINTS ===")
    print("RENDER-DIVERGE = candidate zepto8 bugs to review. NO-GAMEPLAY? = likely needs tailored input.")

if __name__ == "__main__":
    main()
