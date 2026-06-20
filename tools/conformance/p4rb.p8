pico-8 cartridge // http://www.lexaloffle.com/bbs/?pid=0
version 42
__lua__
-- variadic poke4 to 0x5f00 then read back each byte
poke4(0x5f00, 0x0403.0201, 0x0807.0605, 0x0c0b.0a09, 0x100f.0e0d)
local s=""
for a=0x5f00,0x5f0f do s=s..@a.."," end
printh("CONFVAL p4bytes="..s)
