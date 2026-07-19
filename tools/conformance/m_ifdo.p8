pico-8 cartridge // http://www.pico-8.com
version 43
__lua__
-- conformance cart: PICO-8 accepts DO in place of THEN for if/elseif.
-- Measured ground truth (PICO-8 0.2.7a6 -x, 2026-07-19):
--   if c do..end / elseif c do / mixing then+do per clause / if (c) do..end
--   all ACCEPTED; classic short-if `if (c) stmt` unaffected;
--   `while c then` is still a SYNTAX ERROR (asymmetric -- negative case
--   can't live in this cart since it wouldn't load; verified by probe).
-- shrinko8-minified carts rely on this (user report: t2k, 2026-07-19).
function _v(l,v)
 printh("CONFVAL "..l.."="..tostr(v))
end
if 1==1 do _v("ifdo","ok") end
if 1==2 do _v("bad1","x") elseif 1==1 do _v("elseifdo","ok") else _v("bad2","x") end
if 1==2 then _v("bad3","x") elseif 1==1 do _v("mixed","ok") end
if (1==1) do _v("parendo","ok") end
if (1==1) _v("short","ok")
if 1==1 then _v("then","ok") end
if 1==1 do if 2==2 do _v("nested","ok") end end
local n=0
while n<3 do n+=1 end
_v("while_do",n)
extcmd("shutdown")
