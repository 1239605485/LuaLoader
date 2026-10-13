#include "gui_click_state.hpp"
#include "gui_keyboard_bounds.hpp"
#include <cassert>
#include <iostream>
int main(){using namespace lualoader::gui;ClickState c;
 assert(!c.update("a",true,false,true));c.finish(false,true);
 c.update("a",true,true,false);assert(!c.update("b",true,false,true));assert(c.update("a",true,false,true));assert(!c.update("a",true,false,true));c.finish(false,true);
 c.update("a",true,true,false);assert(!c.update("a",false,false,true));c.finish(false,true);
 c.update("old",true,true,false);c.reset();assert(!c.update("old",true,false,true));
 assert(keyboard_contains(640,600,1,1280,720));assert(!keyboard_contains(640,100,1,1280,720));
 assert(keyboard_contains(5,5,2,480,320));
 std::cout<<"PASS: release-only, cross-widget, one event, drag out, reload reset, keyboard bounds\n";
}
