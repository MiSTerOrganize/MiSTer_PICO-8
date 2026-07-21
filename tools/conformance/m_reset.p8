pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- conformance cart: reset() byte semantics over 0x5f00..0x5f7f.
-- Measured ground truth (PICO-8 0.2.7a6 -x, marker-poke probe, 2026-07-21):
--   * resets draw palette (identity + 0x10 transparency on colour 0),
--     screen palette (identity), clip (full screen), pen (6), camera,
--     fillp, screen mode, mouse/preserve flags, audio fx, btnp params,
--     print attrs, memory mapping (00/60/20/80), bitplane mask (0xff),
--     raster mode/bits (0) and raster palette (default gradient table)
--   * 0x5f24 (print start x) and 0x5f26-27 (text cursor) LEFT UNTOUCHED
--   * PRNG state 0x5f44-4b RESEEDED from entropy (marker-independent,
--     varies run to run) -- excluded from the hash below
-- First victim: Run Run Rudolph init_tutorial() froze on nil reset().
function _v(l,v)
 printh("CONFVAL "..l.."="..tostr(v))
end
hd="0123456789abcdef"
function h2(v)
 return sub(hd,flr(v/16)+1,flr(v/16)+1)..sub(hd,v%16+1,v%16+1)
end

-- return values: none
local a,b=reset()
_v("ret",tostr(a)..","..tostr(b))

-- marker-poke the whole region, place the cursor, reset
for i=0,127 do poke(0x5f00+i,0x40+i) end
poke(0x5f24,0x64) poke(0x5f26,19) poke(0x5f27,23)
reset()

-- untouched bytes
_v("start_x",peek(0x5f24))
_v("cur_x",peek(0x5f26))
_v("cur_y",peek(0x5f27))

-- prng reseeded away from the marker (value itself is entropy)
_v("prng_reseeded",peek(0x5f44)!=0x84 or peek(0x5f45)!=0x85 or peek(0x5f46)!=0x86 or peek(0x5f47)!=0x87)

-- hash the deterministic bytes: whole region minus prng (0x44-0x4b)
-- and minus the untouched bytes (0x24,0x26,0x27)
local s=""
for i=0,127 do
 if not (i>=0x44 and i<=0x4b) and i!=0x24 and i!=0x26 and i!=0x27 then
  s=s..h2(peek(0x5f00+i))
 end
end
printh("CONFHASH resetdump="..s)
