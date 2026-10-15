local initialized=false
local selected=0
function on_gui()
 local g=mod.gui
 if not g.begin_scroll then g.text("请使用 LuaLoader 1.9.8"); return end
 if not initialized then
  g.set_pos(180,80);g.set_width(400);g.set_title("滚动测试 1","1");initialized=true
 end
 g.text_at(0,0,"拖动列表或右侧滚动条",{255,230,160,255},0.7)
 local _,sy,_,vh=g.begin_scroll("recipes",0,32,360,190,360,200*44)
 local first=math.max(1,math.floor(sy/44)+1)
 local last=math.min(200,math.ceil((sy+vh)/44))
 for i=first,last do
  local y=(i-1)*44
  g.rect(0,y,340,40,{35,45,65,255})
  g.text_at(8,y+8,"配方 "..i,{240,240,240,255},0.65)
  if g.button_at(220,y+4,104,32,"选择 "..i) then selected=i end
 end
 g.end_scroll()
 local sx,_,vw=g.begin_scroll("items",0,236,360,70,100*80,70)
 for i=math.max(1,math.floor(sx/80)+1),math.min(100,math.ceil((sx+vw)/80)) do
  g.rect((i-1)*80,0,72,52,{50,70,95,255})
  g.text_at((i-1)*80+8,14,tostring(i),{230,230,230,255},0.7)
 end
 g.end_scroll()
 g.text_at(0,318,"已选择："..selected,{220,220,220,255},0.7)
 g.set_content_height(348)
end

-- LuaLoader reads callbacks from the mod table.
mod.on_gui=on_gui
