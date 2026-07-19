pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- conformance cart: decimal number formatting (tostr + string concat).
-- Measured ground truth (PICO-8 0.2.7a6 -x, 2026-07-19):
--   d4 = |frac bits|*10000 >> 16, ROUND HALF-UP on the low 16 bits, but
--   ONLY when d4 > 0 (a round that would manufacture a digit out of
--   ".0000" is suppressed, regardless of the integer part); d4 carries
--   into the integer; sign kept even when all digits vanish ("-0");
--   trailing zeros + dot stripped. tostr() and concat share one path.
function _v(l,v)
 printh("CONFVAL "..l.."="..tostr(v))
end
-- parser bit-exactness for decimal literals (context for older probes)
_v("pA1",tostr(0.0001,true))
_v("pA2",tostr(-0.0001,true))
_v("pA3",tostr(0.5933,true))
-- tostr of hex-exact values: 5th-digit classes, ties, carries, suppression
_v("b97e2",tostr(0x0.97e2))
_v("b0001",tostr(0x0.0001))
_v("b0004",tostr(0x0.0004))
_v("b0006",tostr(0x0.0006))
_v("b0007",tostr(0x0.0007))
_v("b000a",tostr(0x0.000a))
_v("b000b",tostr(0x0.000b))
_v("b0800",tostr(0x0.0800))
_v("b1800",tostr(0x0.1800))
_v("b2800",tostr(0x0.2800))
_v("b0900",tostr(0x0.0900))
_v("b8000",tostr(0x0.8000))
_v("b7999",tostr(0x0.7999))
_v("bffff",tostr(0x0.ffff))
_v("b5555",tostr(0x0.5555))
_v("b50006",tostr(0x0005.0006))
_v("b5ffff",tostr(0x0005.ffff))
_v("b7fffffff",tostr(0x7fff.ffff))
_v("bint",tostr(0x0012.8000))
_v("b97e2neg",tostr(-0x0.97e2))
_v("b0006neg",tostr(-0x0.0006))
_v("bffffneg",tostr(-0x0.ffff))
_v("b50006neg",tostr(-0x0005.0006))
_v("bneg32768",tostr(-32768))
_v("bneg7fff",tostr(-0x7fff.ffff))
-- concat path (same values, must match tostr)
_v("c97e2","z"..0x0.97e2)
_v("c0006","z"..0x0.0006)
_v("c0800","z"..0x0.0800)
_v("cffff","z"..0x0.ffff)
_v("c97e2neg","z"..-0x0.97e2)
_v("cdeclit","z"..-0.0001)
extcmd("shutdown")
