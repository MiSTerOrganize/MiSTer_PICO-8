pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- render-diff DRIVE mechanism check v4: _update60 forces 60fps so _draw is called
-- 1:1 per display frame on both engines (z8headless --frames N == N _update60 calls
-- == official -x loop). f counts in _update60; hash in _draw at checkpoints.
srand(1)
btn=function() return false end
btnp=function() return false end
x=64 y=48 vx=1.5 vy=1 f=0
function _update60()
  f+=1
  x+=vx y+=vy
  if x<8 or x>120 then vx=-vx end
  if y<8 or y>120 then vy=-vy end
  if f>=305 then stop() end
end
function _draw()
  cls(1)
  circfill(x,y,6,8)
  rectfill(0,0,flr(x)\2,4,3)
  line(0,127,flr(x),flr(y),7)
  if f==1 or f==30 or f==120 or f==300 then
    local h=0
    for a=0x6000,0x7fff do h=bxor(rotl(h,3),@a) end
    printh("FBHASH f"..f.."="..tostr(h,true))
  end
end
