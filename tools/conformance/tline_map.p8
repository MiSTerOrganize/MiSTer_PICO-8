pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- z8conformance: tline OOB mx/my must SKIP (not wrap to stale map cells).
-- The oblivion_eve tline class. Value-based: fill a known colour, tline,
-- then pget the pixels tline should / should not have touched.
local function v(l,x) printh("CONFVAL "..l.."="..tostr(x,true)) end
cls(5)                                   -- known background colour 5
mset(0,0,1) mset(1,0,2) mset(0,1,3)      -- some map cells (blank sprites -> colour 0)
-- in-bounds tline: draws sprite pixels (blank sheet -> colour 0) over the 5s
tline(0,0,15,0, 0,0, 0.0625,0)
v("inbounds_px", pget(4,0))              -- expect 0 (drew the in-bounds cell)
-- negative mx/my: OOB, no mask -> MUST skip -> pixel stays 5
tline(0,5,15,5, -1,-1, 0.0625,0.0625)
v("neg_oob_px", pget(4,5))               -- expect 5 (correctly skipped)
-- mx beyond map width: OOB -> MUST skip -> stays 5
tline(0,9,15,9, 200,0, 0.0625,0)
v("hi_oob_px", pget(4,9))                -- expect 5 (correctly skipped)
-- map read-back sanity
v("mget00", mget(0,0))
v("mget10", mget(1,0))
