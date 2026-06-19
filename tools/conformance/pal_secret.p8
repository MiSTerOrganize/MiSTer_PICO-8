pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- z8conformance: pal() draw/screen remap + secret-palette region (value-based).
-- The oblivion_eve secret-palette class. Uses pget/peek (reliable headless
-- channel) instead of framebuffer-hash.
local function v(l,x) printh("CONFVAL "..l.."="..tostr(x,true)) end
cls(0)
-- draw-palette remap: color 3 -> 10. FB must store the REMAPPED index 10.
pal(3,10,0)
rectfill(0,0,15,15,3)
pal()
v("drawpal_px", pget(5,5))           -- expect 10 if draw-pal remap is conformant
-- screen-palette remap is DISPLAY-only: must NOT alter the stored FB index.
pset(20,20,7)
pal(7,2,1)
v("screenpal_px", pget(20,20))       -- expect 7 (screen pal doesn't touch FB)
pal()
-- secret/extended palette region poke + read-back
poke(0x5f10,0x8a) poke(0x5f11,0x03)
v("secret_5f10", peek(0x5f10))
v("secret_5f11", peek(0x5f11))
-- palt / transparency reset state register
v("palt_default", peek(0x5f00))      -- draw-palette register 0 after pal()
-- fillp register
fillp(0b0011010110100011)
v("fillp_reg", peek2(0x5f31))
fillp()
