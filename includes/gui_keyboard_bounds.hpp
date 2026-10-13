#pragma once
#include <algorithm>
namespace lualoader::gui {
inline bool keyboard_contains(float x,float y,float scale,float screen_w,float screen_h){
 const float key=40*scale,gap=6*scale,pad=10*scale;
 const float w=10*key+9*gap+2*pad,h=5*key+4*gap+2*pad;
 const float ox=std::max(0.0f,(screen_w-w)*0.5f),oy=std::max(0.0f,screen_h-h-20*scale);
 return x>=ox&&x<=ox+w&&y>=oy&&y<=oy+h;
}
}
