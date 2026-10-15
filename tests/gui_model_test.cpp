#include "../src/gui.cpp"
#include <cassert>
#include <iostream>
namespace lualoader::lua_engine {void call_on_gui(lua_mod_handle_t*){}}
int main(){using namespace lualoader::gui;
 Viewport v{true,10,10,100,100};float x=0,y=0,w=30,h=30;assert(v.crop(x,y,w,h)&&x==10&&y==10&&w==20&&h==20);
 assert(!v.intersects(111,12,10,10));assert(v.contains(10,10));assert(!v.covers(9,10,10,10));
 ScrollMotion m;m.begin(0,0);m.move(0,-100,0,200,100,100,6);assert(m.dragged&&m.y==100);m.move(0,-500,0,200,100,100,6);assert(m.y==200);
 m.begin(0,0,true,false);m.move(10,0,200,0,100,100,6);assert(m.x==20);
 g_scale=1;g_screen_w=1280;g_screen_h=720;g_current_mod="test";g_render_owner="test";g_input_owner="test";
 Ctx ctx;ctx.ox=100;ctx.oy=100;ctx.cw=500;g_ctx=&ctx;g_recording=true;
 auto *L=luaL_newstate();
 auto begin=[&](){lua_settop(L,0);lua_pushstring(L,"list");for(double n:{0.,0.,100.,100.,100.,1000.})lua_pushnumber(L,n);assert(l_gui_begin_scroll(L)==4);};
 g_mouse_x=110;g_mouse_y=110;g_mouse_pressed=true;g_mouse_down=true;begin();assert(g_ctx!=&ctx);end_scroll();assert(g_ctx==&ctx);
 g_mouse_pressed=false;g_mouse_y=10;begin();assert(g_scroll_drag);assert(g_scrolls["test#list"].y==100);end_scroll();
 g_mouse_down=false;g_mouse_released=true;begin();assert(!widget_click("a",0,0,true));end_scroll();assert(g_active_scroll.empty());
 lua_settop(L,0);assert(luaL_dostring(L,"return {r=255,g=20,b=0,a=128}")==0);auto color=lua_color(L,-1);assert(color.r==255&&color.g==20&&color.b==0&&color.a==128);
 lua_settop(L,0);g_mouse_pressed=true;g_mouse_down=true;g_mouse_released=false;g_mouse_y=110;g_scroll_drag=false;g_scrolls["test#list"].y=0;begin();
 lua_settop(L,0);lua_pushstring(L,"inner");for(double n:{0.,0.,50.,50.,50.,500.})lua_pushnumber(L,n);l_gui_begin_scroll(L);assert(g_active_scroll=="test#inner");end_scroll();end_scroll();g_active_scroll.clear();
 ScrollMotion thumb;thumb.begin(0,0,true,false);thumb.move(84,0,900,0,84,1,6);assert(thumb.x==900);
 lua_close(L);g_ctx=nullptr;g_recording=false;g_cmds.clear();g_scroll_drag=false;
 lualoader::lua_mod_handle_t a{},b{};a.mod_id="a";a.gui_ref=0;b.mod_id="b";b.gui_ref=0;lualoader::core::lua_mod_handles["a"]=&a;lualoader::core::lua_mod_handles["b"]=&b;
 g_windows["a"].collapsed=true;g_windows["b"].collapsed=true;merge_icon("a","b");assert(g_folders.size()==1&&grouped_icon("a")&&grouped_icon("b"));
 g_windows["a"].collapsed=false;assert(!grouped_icon("a"));g_windows["a"].collapsed=true;
 auto root=std::filesystem::temp_directory_path()/"lualoader-gui-model-test";std::filesystem::create_directories(root);g_layout_path=(root/"gui_layout.json").string();save_layout();load_layout(root.string());assert(g_folders.size()==1&&g_folders[0].members.size()==2);
 g_folders[0].members.erase(g_folders[0].members.begin());assert(!grouped_icon("a")&&!grouped_icon("b"));
 std::filesystem::remove_all(root);lualoader::core::lua_mod_handles.clear();
 std::cout<<"PASS viewport, scroll clamp, drag cancellation, container restoration, folder membership and persistence\n";
}
