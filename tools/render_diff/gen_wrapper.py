#!/usr/bin/env python3
"""
gen_wrapper.py -- render-diff wrapper generator (Milestone 2).

Takes a real PICO-8 cart (.p8 or .p8.png) and emits a THROWAWAY test wrapper .p8
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

def harness(stop_at):
    # PREPEND (before cart code): override flip() -- the UNIVERSAL per-frame hook.
    # The engine calls global flip() to present each frame for modern _update/_draw
    # carts, AND old top-level flip-loop carts call it manually. flip == one display
    # frame on both engines, so cadence aligns automatically (no _update 30/60 issue).
    # At flip, 0x6000 holds the completed frame -> hash it at checkpoints.
    cks = "".join("[%d]=1," % c for c in CHECKPOINTS)
    # Token-minified: comments are FREE in PICO-8 (not counted), but code tokens are,
    # and prepending pushes near-8192-token carts over the official limit. This keeps
    # input + render(FB) + audio(stat) hashing in the fewest tokens. Carts still over
    # the limit on official are flagged WRAP-OVERSIZE (can't auto-wrap with extra Lua).
    # __m: scripted generic advance input (pulse X/O + hold right); btn/btnp alias it.
    # t/time: frame-based deterministic. flip: universal per-frame hook (FB+audio hash).
    return f"""-- z8render-diff harness (auto-injected, throwaway; cart untouched)
__f=0
__ck={{{cks}}}
__rf=flip
srand(1)
t=function() return __f/60 end
time=t
__m=function(i)
 local m=0
 local p=__f%48
 if p<3 then m=32 end
 if p>7 and p<11 then m=16 end
 if __f>90 and __f<240 then m=m|2 end
 if i then return (m>>i)&1==1 end
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
 if __rf then __rf() end
 if __f>={stop_at} then stop() end
end
"""

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

def inject(p8_path, out_path, stop_at):
    with open(p8_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    # find __lua__ then the next section marker (__xxx__) after it
    lua_i = next((i for i, l in enumerate(lines) if l.strip() == "__lua__"), None)
    if lua_i is None:
        raise RuntimeError("no __lua__ section")
    # PREPEND the harness right after the __lua__ marker, before the cart's code,
    # so the flip() override is in place before the cart runs (catches top-level
    # flip-loop carts whose code never returns to an appended harness).
    h = harness(stop_at)
    insert_at = lua_i + 1
    out = lines[:insert_at] + [h if h.endswith("\n") else h + "\n"] + lines[insert_at:]
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
    p8, tmp = to_p8(cart, shrinko)
    try:
        inject(p8, out, stop_at)
    finally:
        if tmp and os.path.exists(tmp):
            os.remove(tmp)
    print(f"wrapper -> {out}  (checkpoints {CHECKPOINTS}, stop {stop_at})")

if __name__ == "__main__":
    main()
