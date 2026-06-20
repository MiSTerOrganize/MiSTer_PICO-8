pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- z8conformance mechanism check: draw a deterministic scene, hash the raw
-- 4bpp framebuffer (0x6000..0x7fff), printh the hash. Must produce an
-- IDENTICAL hash on PICO-8 (-x) and on z8headless (zepto8). A
-- divergence on a tricky-API cart = a zepto8 conformance bug.
cls(0)
for i=0,15 do rectfill(i*8,0,i*8+7,7,i) end
circfill(64,64,30,8)
line(0,0,127,127,7)
spr(0,40,40)
print("conf",10,100,7)
local h=0
for a=0x6000,0x7fff do h=bxor(rotl(h,3),@a) end
printh("CONFHASH="..tostr(h,true))
