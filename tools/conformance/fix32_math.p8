pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- z8conformance: fix32 (16.16) arithmetic edge cases — the Virtua Racing class.
-- Emits CONFVAL lines (value tests, not framebuffer). Any divergence between
-- PICO-8 and z8headless = a zepto8 fix32 conformance bug.
local function e(l,x) printh("CONFVAL "..l.."="..tostr(x,true)) end
e("div",100/3)
e("divneg",-100/3)
e("mul",1000*1000)        -- 16.16 overflow wrap
e("mulneg",-1000*1000)
e("mulfrac",0x0.8*0x0.8)
e("sqrt2",sqrt(2))
e("sqrt0",sqrt(0))
e("sin",sin(0.1))
e("cos",cos(0.1))
e("atan2",atan2(3,4))
e("flr",flr(3.7))
e("flrneg",flr(-3.7))
e("ceil",ceil(-3.2))
e("mod",10.5%3)
e("modneg",-10.5%3)
e("shl",1<<4)
e("ashr",-256>>2)         -- arithmetic right shift (sign-extend)
e("lshr",256>>>2)         -- logical right shift
e("rotl",rotl(0x1234.5678,8))
e("rotr",rotr(0x1234.5678,8))
e("band",band(0xabcd,0x0f0f))
e("bxor",bxor(0xffff,0x1234))
e("maxfrac",max(0x.0001,0x.0002))
e("absmin",abs(-0x7fff.ffff))
e("tostr_neg",tostr(-1.5,true))
