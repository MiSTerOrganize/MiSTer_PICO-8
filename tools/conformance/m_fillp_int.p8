pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- conformance cart: integrated fillp colours (0x5f34) truth table.
-- Measured ground truth (reference 0.2.7a6 -x, 2026-07-21):
--   * pattern bits in a colour value are observed ONLY when BOTH the
--     observe bit 0x1000.0000 is set AND 0x5f34 = 1 (v7/v8 dither;
--     v1-v3/v9 stay solid + capture nothing)
--   * this applies EQUALLY to color()-then-draw and draw-call colour
--     args (v7 == v8) -- zepto8 historically dropped the color() case
--     (uint8_t binding truncated the pattern bits): Virtua Racing sets
--     face colours via color(), so its cliffs/bridge rendered SOLID
--     instead of dithered. Fixed 2026-07-21.
--   * state fillp() + plain colours dither regardless (v4/v5)
function _v(l,v) printh("CONFVAL "..l.."="..tostr(v)) end
hd="0123456789abcdef"
function rowhash(x0,y0)
 local s=""
 for y=0,3 do for x=0,7 do s=s..sub(hd,pget(x0+x,y0+y)%16+1,pget(x0+x,y0+y)%16+1) end end
 return s
end
function regs(l)
 _v(l.."_regs",tostr(peek(0x5f31))..","..tostr(peek(0x5f32))..","..tostr(peek(0x5f33))..","..tostr(peek(0x5f34)))
end

poke(0x5f34,1)
-- v1: color() with pattern in FRACTION
cls() fillp() color(0x21.a5a5) rectfill(0,0,7,3)
_v("v1",rowhash(0,0)) regs("v1")
-- v2: color() with pattern in INTEGER half (0xa5a5.0021? colours in fraction??)
cls() fillp() color(0xa5a5.0021) rectfill(0,0,7,3)
_v("v2",rowhash(0,0)) regs("v2")
-- v3: rectfill arg, pattern in fraction, colours as single nibble pair 0x21
cls() fillp() rectfill(0,0,7,3,0x21.a5a5)
_v("v3",rowhash(0,0)) regs("v3")
-- v4: known idiom fillp state + plain colour (sanity: state fillp works at all)
cls() fillp(0xa5a5) rectfill(0,0,7,3,0x21)
_v("v4",rowhash(0,0)) regs("v4")
-- v5: fillp(0xa5a5) has set state; does integrated colour OVERRIDE it? draw plain 7
cls() rectfill(0,0,7,3,7)
_v("v5",rowhash(0,0))
fillp()
-- v6: color() with pattern+colours via known cart idiom 0x0.a5a5 added to colour 0x21? (= v1 duplicate but via pset loop)
cls() color(0x21.a5a5) for x=0,7 do for y=0,3 do pset(x,y) end end
_v("v6",rowhash(0,0))
-- v7: 0x5f34=1 AND observe bit 0x1000.0000 in a color() call
cls() fillp() color(0x1000.0000+0x21.a5a5) rectfill(0,0,7,3)
_v('v7',rowhash(0,0)) regs('v7')
-- v8: same but as the draw-call colour arg
cls() fillp() rectfill(0,0,7,3,0x1000.0000+0x21.a5a5)
_v('v8',rowhash(0,0)) regs('v8')
-- v9: 0x5f34=0 + observe bit via color()
poke(0x5f34,0)
cls() fillp() color(0x1000.0000+0x21.a5a5) rectfill(0,0,7,3)
_v('v9',rowhash(0,0)) regs('v9')
