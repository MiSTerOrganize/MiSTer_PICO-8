#!/usr/bin/env python3
"""
gen_wrapper.py -- render-diff wrapper generator (Milestone 2).

Takes a PICO-8 cart (.p8 or .p8.png) and emits a THROWAWAY test wrapper .p8
that drives the cart frame-by-frame at a forced 60fps and printh's a framebuffer
hash at checkpoint frames (the validated M1 recipe). Run the wrapper on official
PICO-8 (-x, ground truth) and z8headless (ours); diff the FBHASH lines.

The user's cart is NEVER modified -- we read it (and, for .p8.png, decompress a
copy via shrinko8) and write a separate wrapper file. shrinko8 = gold-standard
decompressor (preserves __gfx__/__map__/__sfx__/__music__).

Usage:
  python gen_wrapper.py <cart.p8|cart.p8.png> <out_wrapper.p8> [--shrinko DIR] [--frames N]
"""
import sys, os, subprocess, tempfile

CHECKPOINTS = [1, 2, 8, 30, 60, 120, 240, 300]   # display frames to hash
DEFAULT_STOP = max(CHECKPOINTS) + 5

def harness(stop_at, noinput=False, dumpframe=0, seq=0, dwin=None, inp=False):
    if dwin and inp:
        # WINDOW dump WITH scripted gameplay input -- to reach + diff GAMEPLAY (resolves
        # no-input state-drift FPs + catches gameplay-gated render bugs like VR's track).
        # Both engines get the IDENTICAL deterministic input; the pixel-window classifier
        # tolerates small input-timing drift (anchor vs whole window), so a persistent
        # gameplay render bug shows high residual while position drift stays low.
        # Mask: mash X to start/skip menus, then hold right + periodic jump(X)/up.
        s, e = dwin
        return (f"""-- z8render-diff WINDOW+INPUT harness (throwaway; cart untouched)
__f=0
__rf=flip
srand(1)
t=function() return __f/60 end
time=t
__im=function(i)
 local m=0
 if __f<16 then m=32 end
 if __f>=16 then
  m=2
  if __f%16<3 then m=m|32 end
  if __f%32<4 then m=m|4 end
 end
 if i then return (m>>i)&1==1 end
 return m
end
btn=__im
btnp=__im
flip=function()
 __f+=1
 if __f>={s} and __f<={e} then
  local x="0123456789abcdef"
  for r=0,127 do
   local ss=""
   for c=0,63 do local b=@(0x6000+r*64+c) ss=ss..sub(x,b\\16+1,b\\16+1)..sub(x,b%16+1,b%16+1) end
   printh("FBDUMP "..__f.." "..r.." "..ss)
  end
 end
 if __rf then __rf() end
 if __f>{e} then stop() end
end
""", "")
    if dwin:
        # WINDOW dump: dump the full 128x128 framebuffer for EVERY frame in [s..e],
        # frame-tagged ("FBDUMP <frame> <row> <hex>"), no input. The pixel-window
        # classifier compares anchor-frame pixels against the OTHER engine's whole
        # window -> a real bug's anchor matches no nearby frame (large residual);
        # an animation/phase FP's anchor pixel-matches a frame a few steps away.
        s, e = dwin
        return (f"""-- z8render-diff WINDOW-dump harness (throwaway; cart untouched)
__f=0
__rf=flip
srand(1)
t=function() return __f/60 end
time=t
__m=function(i) if i then return false end return 0 end
btn=__m
btnp=__m
flip=function()
 __f+=1
 if __f>={s} and __f<={e} then
  local x="0123456789abcdef"
  for r=0,127 do
   local ss=""
   for c=0,63 do local b=@(0x6000+r*64+c) ss=ss..sub(x,b\\16+1,b\\16+1)..sub(x,b%16+1,b%16+1) end
   printh("FBDUMP "..__f.." "..r.." "..ss)
  end
 end
 if __rf then __rf() end
 if __f>{e} then stop() end
end
""", "")
    if seq:
        # SEQUENCE mode: hash 0x6000 EVERY frame 1..seq (no checkpoint gating), no
        # dump. Used by the Tier-2 trustworthiness discriminator: a candidate whose
        # z8 frame-hash SET ~= official's (just phase-shifted) is an animation/pacing
        # FALSE POSITIVE; largely-disjoint sets = a REAL render divergence. Input off.
        return (f"""-- z8render-diff SEQ harness (throwaway; cart untouched)
__f=0
__rf=flip
srand(1)
t=function() return __f/60 end
time=t
__m=function(i) if i then return false end return 0 end
btn=__m
btnp=__m
flip=function()
 local h=0
 for a=0x6000,0x7fff do h=bxor(rotl(h,3),@a) end
 local d=0
 for i=16,23 do d=d*64+stat(i) end
 __f+=1
 printh("FBHASH f".. __f .."="..tostr(h,true))
 printh("AUDHASH f".. __f .."="..tostr(d,true))
 if __rf then __rf() end
 if __f>={seq} then stop() end
end
""", "")
    # Trustworthy combined harness -> returns (PREPEND, APPEND).
    # PREPEND (before cart): flip() hook for top-level flip-loop carts (which never
    #   return to appended code). Counts+hashes per flip ONLY while __tk is false.
    # APPEND (after cart; runs only for modern carts whose top-level returns):
    #   takes full control -- nils the cart's _update/_update60 and re-defines them so
    #   the cart's update runs EXACTLY ONCE per frame on BOTH engines (deterministic
    #   1:1 cadence). This eliminates the _update-vs-flip frame-PACING that caused the
    #   Wander starfield false-positive. Driven by the engine's frame loop (--frames N)
    #   so it stays within z8's per-frame budget (a top-level for-loop hit a cutoff).
    # Comments are FREE in PICO-8 tokens; code tokens are not (near-8192-token carts ->
    # WRAP-OVERSIZE). __hs(): shared FB+audio hash (deduped). t/time frame-based; srand
    # fixed; btn/btnp = scripted advance input (--noinput zeros it for secondary triage).
    cks = "".join("[%d]=1," % c for c in CHECKPOINTS)
    maskbody = "" if noinput else (
        " local p=__f%48\n"
        " if p<3 then m=32 end\n"
        " if p>7 and p<11 then m=16 end\n"
        " if __f>90 and __f<240 then m=m|2 end\n")
    dumpblock = "" if not dumpframe else (
        f" if __f=={dumpframe} then\n"
        '  local x="0123456789abcdef"\n'
        "  for r=0,127 do\n"
        '   local s=""\n'
        "   for c=0,63 do local b=@(0x6000+r*64+c) s=s..sub(x,b\\16+1,b\\16+1)..sub(x,b%16+1,b%16+1) end\n"
        '   printh("FBDUMP "..r.." "..s)\n'
        "  end\n"
        "  stop()\n"
        " end\n")
    # Flip-only harness (PREPEND only): proven to work for ALL cart structures (the
    # engine calls global flip() to present for modern _update/_draw carts AND flip-loop
    # carts call it manually). Frame-pacing scatter (e.g. starfields) is handled NOT by
    # cadence surgery but by the two-tier magnitude triage (fbdiff cluster analysis):
    # exact-hash pass -> candidates -> FB-dump + cluster -> SCATTER-FP vs CLUSTERED-REAL.
    pre = f"""-- z8render-diff harness (throwaway; cart untouched)
__f=0
__ck={{{cks}}}
__rf=flip
srand(1)
t=function() return __f/60 end
time=t
__m=function(i)
 local m=0
{maskbody} if i then return (m>>i)&1==1 end
 return m
end
btn=__m
btnp=__m
flip=function()
 local h=0
 for a=0x6000,0x7fff do h=bxor(rotl(h,3),@a) end
 local d=0
 for i=16,23 do d=d*64+stat(i) end
 __f+=1
 if __ck[__f] then
  printh("FBHASH f".. __f .."="..tostr(h,true))
  printh("AUDHASH f".. __f .."="..tostr(d,true))
 end
{dumpblock} if __rf then __rf() end
 if __f>={stop_at} then stop() end
end
"""
    return pre, ""

def to_p8(cart, shrinko_dir):
    """Return path to a full .p8 (decompress .p8.png via shrinko8 to a temp file)."""
    if cart.lower().endswith(".p8"):
        return cart, None
    tmp = tempfile.NamedTemporaryFile(suffix=".p8", delete=False).name
    sh = os.path.join(shrinko_dir, "shrinko8.py")
    r = subprocess.run([sys.executable, sh, cart, tmp],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) == 0:
        raise RuntimeError(f"shrinko8 failed on {cart}: {r.stderr[:300]}")
    return tmp, tmp

def inject(p8_path, out_path, stop_at, noinput=False, dumpframe=0, seq=0, dwin=None, inp=False):
    with open(p8_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    # find __lua__ then the next section marker (__xxx__) after it
    lua_i = next((i for i, l in enumerate(lines) if l.strip() == "__lua__"), None)
    if lua_i is None:
        raise RuntimeError("no __lua__ section")
    # next section marker (__gfx__ etc.) after __lua__ = end of the code section
    nxt = next((i for i in range(lua_i + 1, len(lines)) if l_is_section(lines[i])),
               len(lines))
    pre, post = harness(stop_at, noinput, dumpframe, seq, dwin, inp)
    if not pre.endswith("\n"): pre += "\n"
    if not post.endswith("\n"): post += "\n"
    # PREPEND pre (right after __lua__, before cart code -> catches flip-loop carts),
    # APPEND post (end of code section, before next __section__ -> modern-cart driver).
    out = (lines[:lua_i + 1] + [pre] + lines[lua_i + 1:nxt] + [post] + lines[nxt:])
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)

def l_is_section(line):
    s = line.strip()
    return s.startswith("__") and s.endswith("__") and len(s) > 4

def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); sys.exit(2)
    cart, out = a[0], a[1]
    shrinko = a[a.index("--shrinko") + 1] if "--shrinko" in a else \
              "C:/Users/miste/AppData/Local/Temp/shrinko8"
    stop_at = int(a[a.index("--frames") + 1]) if "--frames" in a else DEFAULT_STOP
    noinput = "--noinput" in a
    dumpframe = int(a[a.index("--dumpframe") + 1]) if "--dumpframe" in a else 0
    seq = int(a[a.index("--seq") + 1]) if "--seq" in a else 0
    dwin = (int(a[a.index("--dumpwin") + 1]), int(a[a.index("--dumpwin") + 2])) if "--dumpwin" in a else None
    inp = "--play" in a
    p8, tmp = to_p8(cart, shrinko)
    try:
        inject(p8, out, stop_at, noinput, dumpframe, seq, dwin, inp)
    finally:
        if tmp and os.path.exists(tmp):
            os.remove(tmp)
    print(f"wrapper -> {out}  (checkpoints {CHECKPOINTS}, stop {stop_at})")

if __name__ == "__main__":
    main()
