-- Generic PNG skin example. Every asset lives in this mod, not in the loader.
mod.meta={pkg_id="lzup.lua.taskpanel",version="1.4.0"}
local g=mod.gui
local page,selected="home",1
local assets,placed={},false
local theme=1
local theme_names={"黑金雕花","金色纸张","冰蓝纸张"}
local tone_main={236,212,161,255}
local tone_muted={179,168,148,255}
local image_warning_logged=false
local tasks={
 {"准备出发","收集木材，制作工作台。","寻找树木收集木材。制作工作台并放置在安全位置，为冒险准备工具。"},
 {"建立住所","建造一间安全的房屋。","准备背景墙、光源、桌椅和门。使用住房查询确认入住条件。"},
 {"地下探索","准备火把，寻找矿石。","带上火把和恢复用品。沿洞穴探索，注意落差，找到矿石后安全返回。"}
}
local function asset(name)
 if not assets[name] then assets[name]=g.load_image("lib/ui/"..name..".png") end
 return assets[name]
end
local function button(y,label)
 local skin=theme==1 and assets.button_ornate or theme==3 and assets.button_blue or assets.button
 if skin then return g.image_button(skin,12,y,252,42,label,12) end
 return g.button_at(12,y,252,42,label)
end
local function apply_skin()
 local panel=theme==1 and assets.panel_ornate or theme==3 and assets.panel_blue or assets.panel
 local book=theme==1 and assets.book_ornate or theme==3 and assets.book_blue or assets.book
 tone_main=theme==1 and {236,212,161,255} or {106,63,31,255}
 tone_muted=theme==1 and {179,168,148,255} or {147,109,65,255}
 g.set_style({text=tone_main,panel={43,38,32,255},title={104,76,44,255}})
 if panel and book then g.set_skin(panel,theme==1 and 300 or 18,book,48,theme==1 and 36 or 0) end
end
local function toggle_skin()
 theme=theme%3+1
 apply_skin()
end
function on_gui()
 if type(g)~="table" or type(g.load_image)~="function" or (g.image_api_version or 0)<2 then
  if g and g.text then g.text("需要 LuaLoader 1.9.2-ui-test") end
  return
 end
 if not placed then
  g.set_pos(300,100);g.set_width(300)
  g.set_title("冒险手册","册")
  g.set_style({text={93,60,35,255},panel={43,38,32,255},title={104,76,44,255}})
  for _,name in ipairs({"panel","book","button","close","white","panel_blue","book_blue","button_blue","panel_ornate","book_ornate","button_ornate","close_ornate"})do asset(name) end
  apply_skin()
  placed=true;g.set_collapsed(true);return
 end
 local close=theme==1 and assets.close_ornate or assets.close
 if close and g.image_button(close,244,0,30,30,"",0) then g.set_collapsed(true) end
 if page~="home" then
  local skin=theme==1 and assets.button_ornate or theme==3 and assets.button_blue or assets.button
  if skin and g.image_button(skin,184,0,52,30,"换肤",10) then toggle_skin() end
 end
 if page=="home" then
  local book=theme==1 and assets.book_ornate or theme==3 and assets.book_blue or assets.book
  if book then g.image_fit(book,18,0,54,54,"contain") end
  g.text_at(86,8,"冒险启程",tone_main,0.85)
  g.text_at(86,35,"你的任务手册",tone_muted,0.6)
  if button(76,"浏览任务") then page="list" end
  if button(132,"换肤："..theme_names[theme]) then
   toggle_skin()
  end
  if assets.white then
   g.image(assets.white,24,184,228,5,{162,123,67,255})
   g.image(assets.white,24,184,76,5,{219,176,77,255})
  end
  g.text_at(42,201,"介绍示例 · 不发放奖励",tone_muted,0.6)
  g.set_content_height(235)
 elseif page=="list" then
  g.text_at(12,0,"选择一项冒险",tone_main,0.8)
  for i,task in ipairs(tasks)do if button(42+(i-1)*52,task[1])then selected=i;page="detail" end end
  if button(208,"返回首页")then page="home" end
  g.set_content_height(267)
 else
  local task=tasks[selected]
  g.text_at(12,0,task[1],tone_main,0.9)
  g.set_cursor(12,42);g.text_wrapped("目标："..task[2]);g.spacing();g.text_wrapped(task[3])
  if button(222,"返回任务列表")then page="list" end
  g.set_content_height(280)
 end
 if type(g.image_status)=="function" and assets.panel then
  local status=g.image_status(theme==1 and assets.panel_ornate or theme==3 and assets.panel_blue or assets.panel)
  if status:sub(1,6)=="failed" then
   g.text_at(12,290,"图片未加载，请查看日志",{255,180,60,255},0.65)
   g.set_content_height(325)
   if not image_warning_logged and type(mod.info)=="function" then
    mod.info("[TaskPanel] "..status);image_warning_logged=true
   end
  end
 end
end
mod.on_gui=on_gui
