pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- z8conformance: extended-memory (0x8000+) peek/poke + the $/%/@ shorthands.
-- The peek-shorthand-extended-memory class (luaV_peek vs api_peek divergence).
-- Emits CONFVAL lines.
local function e(l,x) printh("CONFVAL "..l.."="..tostr(x,true)) end
poke(0x8000,0x12) poke(0x8001,0x34) poke(0x8002,0xab)
poke2(0x8010,0x1234)
poke4(0x8020,0x1234.5678)
e("peek8000",peek(0x8000))
e("at8000",@0x8000)            -- @ shorthand (peek1)
e("at8002",@0x8002)
e("peek2_8010",peek2(0x8010))
e("pct8010",%0x8010)          -- % shorthand (peek2)
e("peek4_8020",peek4(0x8020))
e("dol8020",$0x8020)          -- $ shorthand (peek4)
e("oob_ffff",peek(0xffff))    -- top of extended mem
poke(0x8030,0x80) poke(0x8031,0x00)
e("hi_bit2",%0x8030)          -- sign handling of high bit via peek2 shorthand
-- general (non-extended) sanity
poke(0x4300,0x55)
e("at4300",@0x4300)
