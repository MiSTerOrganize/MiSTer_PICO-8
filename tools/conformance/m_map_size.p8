pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- conformance cart: map() celw/celh defaults + shared-row reach + nil-vs-0.
-- Measured ground truth (PICO-8 0.2.7a6 -x, 2026-07-19):
--   * omitted or NIL celw/celh each independently default to the FULL map
--     dimension, INCLUDING the shared gfx/map rows (32..63)
--   * an explicit 0 or negative celw/celh draws NOTHING
-- The old "defaults to 128,32" documentation is wrong; carts that scroll a
-- no-arg map() past row 31 (On A Roll, Mina, Bomba, Medusa...) rely on this.
function _v(l,v)
 printh("CONFVAL "..l.."="..tostr(v))
end
-- sprite 1 = solid colour 8
for r=0,7 do
 for b=0,3 do
  poke(r*64+4+b,0x88)
 end
end
mset(2,20,1)  -- normal map region (row < 32)
mset(2,40,1)  -- shared gfx/map region
mset(2,50,1)  -- shared gfx/map region
_v("mget20",mget(2,20)) _v("mget40",mget(2,40)) _v("mget50",mget(2,50))
cls() camera(0,160) map() camera()
_v("a_row20_noargs",pget(20,4))
cls() camera(0,320) map() camera()
_v("b_row40_noargs",pget(20,4))
cls() camera(0,384) map() camera()
_v("c_row50_noargs",pget(20,20))
cls() camera(0,384) map(0,0,0,0,128,64) camera()
_v("d_row50_celh64",pget(20,20))
cls() camera(0,0) map(0,32,0,0,128,32)
_v("e_row40_celxy",pget(20,68))
cls() camera(0,320) map(0,0,0,0,128) camera()
_v("f_row40_celwonly",pget(20,4))
cls() camera(0,320) map(0,0,0,0,nil,64) camera()
_v("h_row40_nilcelw",pget(20,4))
cls() camera(0,320) map(0,0,0,0,16) camera()
_v("g_row40_celw16",pget(20,4))
cls() camera(0,320) map(0,0,0,0,0,64) camera()
_v("i_celw0",pget(20,4))
cls() camera(0,320) map(0,0,0,0,128,0) camera()
_v("j_celh0",pget(20,4))
cls() camera(0,320) map(0,0,0,0,-1,64) camera()
_v("k_celwneg",pget(20,4))
cls() camera(0,320) map(0,0,0,0,128,-1) camera()
_v("l_celhneg",pget(20,4))
cls() camera(0,160) map(0,0,0,0,128,21) camera()
_v("n_celh21_row20",pget(20,4))
-- nil stays PRESENT (coerces to 0/false) for other optional args -- measured:
srand(1) local ra=rnd() srand(1) local rb=rnd(nil)
_v("rnd_nil_eq_absent",ra==rb)
_v("rnd_nil",rb)
cls() pal(3,9) pal(nil,nil) pset(6,6,3)
_v("pal_nilnil_no_reset",pget(6,6))
pal()
local pk=peek(0x5f26,nil)
_v("peek_nilcount",pk==nil)
_v("btn_nil",btn(nil))
extcmd("shutdown")
