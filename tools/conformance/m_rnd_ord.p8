pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- conformance cart: rnd(table) element selection + ord() edge cases.
-- Measured ground truth (PICO-8 0.2.7a6 -x, 2026-07-19):
--   * rnd(tbl) consumes exactly ONE prng draw and picks the 0-based index
--     (prng_a >> 8) % #tbl -- a DIFFERENT bit slice than rnd(n), so
--     t[flr(rnd(#t))+1] gives the wrong element under the same seed
--   * rnd({}) returns nil
--   * ord(nil)/ord()/ord("")/out-of-range return NO values (nil), never zeros
function _v(l,v)
 printh("CONFVAL "..l.."="..tostr(v))
end
local t10={1,2,3,4,5,6,7,8,9,10}
srand(1) local e="" for i=1,8 do e=e..tostr(rnd(t10)).."," end
_v("tbl10_s1",e)
srand(42) local e2="" for i=1,8 do e2=e2..tostr(rnd(t10)).."," end
_v("tbl10_s42",e2)
srand(1) local q="" for i=1,8 do q=q..tostr(rnd({0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}))..","end
_v("tbl16_s1",q)
srand(1) local s2="" for i=1,8 do s2=s2..tostr(rnd({1,2})).."," end
_v("tbl2_s1",s2)
srand(1) local z="" for i=1,6 do z=z..tostr(rnd({10,20,30})).."," end
_v("tbl3_s1",z)
srand(1) rnd({9,9,9})
_v("consumes_one",tostr(rnd(),true))
_v("tbl_empty",rnd({}))
_v("ord_nil",ord(nil))
local a,b,c=ord(nil,1,3)
_v("ord_nil13",tostr(a).." "..tostr(b).." "..tostr(c))
_v("ord_noargs",ord())
_v("ord_empty",ord(""))
_v("ord_oob",ord("ab",5))
local d,e3=ord("ab",1,5)
_v("ord_clamp",tostr(d).." "..tostr(e3))
_v("ord_num",ord(65))
extcmd("shutdown")
