#include "gui_icon_gesture.hpp"
#include <cassert>
#include <iostream>
using lualoader::gui::IconGesture;
int main(){IconGesture g;
 g.update(20,20,true,true,false,true,10,10,6);
 assert(g.update(21,20,false,false,true,true,10,10,6).tap);
 g.update(20,20,true,true,false,true,10,10,6);
 auto r=g.update(60,80,false,true,false,false,10,10,6);
 assert(r.move && r.x==50 && r.y==70);
 assert(!g.update(60,80,false,false,true,true,50,70,6).tap);
 g.update(20,20,true,true,false,true,10,10,6);
 g.update(40,40,false,true,false,true,10,10,6);
 g.update(20,20,false,true,false,true,30,30,6);
 assert(!g.update(20,20,false,false,true,true,10,10,6).tap);
 assert(!g.update(20,20,false,false,true,true,10,10,6).tap);
 g.update(20,20,true,true,false,false,10,10,6);assert(!g.active);
 g.update(20,20,true,true,false,true,10,10,6);
 auto final=g.update(40,20,false,false,true,true,10,10,6);assert(final.move && !final.tap);
 std::cout<<"PASS: tap, drag outside, drag back, release without press, outside press\n";
}
