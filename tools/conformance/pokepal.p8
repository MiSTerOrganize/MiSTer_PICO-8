pico-8 cartridge // http://www.lexaloffle.com/bbs/?pid=0
version 42
__lua__
function e(l,x) printh("CONFVAL "..l.."="..x) end
cls(0)
pal() rectfill(0,0,4,4,8) e("base", pget(2,2))
pal() pal(8,12) rectfill(0,0,4,4,8) e("pal_set", pget(2,2))
pal() poke(0x5f00+8,12) rectfill(0,0,4,4,8) e("poke_set", pget(2,2))
pal() poke4(0x5f00,0x03020100,0x07060504,0x0b0a090c,0x0f0e0d0c) rectfill(0,0,4,4,8) e("poke4_set", pget(2,2))
