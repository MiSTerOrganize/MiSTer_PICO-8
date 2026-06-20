pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- z8conformance: rnd() sequence determinism vs PICO-8 (srand-seeded).
-- If these diverge, zepto8's PRNG differs from official -> every rnd-driven cart
-- (starfields, particles, procedural) renders differently. Root-cause check for
-- the Wander the Cosmos starfield divergence.
local function e(l,x) printh("CONFVAL "..l.."="..tostr(x,true)) end
srand(1)
for i=1,8 do e("r"..i, rnd()) end
srand(42)
for i=1,4 do e("a"..i, rnd(100)) end
srand(1)
for i=1,4 do e("i"..i, flr(rnd(1000))) end
srand(0)
e("z1", rnd())
e("z2", rnd(1))
