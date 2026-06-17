pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- harness test fixture (synthetic, not user content)
-- deterministic per-frame output to verify the headless dumper.
f=0
function _update()
 f+=1
end
function _draw()
 cls(1)
 for i=0,15 do rectfill(i*8,0,i*8+7,12,i) end
 local a=f/60
 circfill(64+cos(a)*40,70,18,8)
 rectfill(20,100,20+f%80,110,11)
 line(0,127,127,40,12)
 print("frame "..f,4,40,7)
end
