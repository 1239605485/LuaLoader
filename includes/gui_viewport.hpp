#pragma once
#include <algorithm>
#include <cmath>
namespace lualoader::gui {
struct Viewport {
 bool enabled=false;float x=0,y=0,w=0,h=0;
 bool contains(float px,float py)const{return !enabled||(px>=x&&py>=y&&px<x+w&&py<y+h);}
 bool intersects(float px,float py,float pw,float ph)const{return !enabled||(px<x+w&&py<y+h&&px+pw>x&&py+ph>y);}
 bool covers(float px,float py,float pw,float ph)const{return !enabled||(px>=x&&py>=y&&px+pw<=x+w&&py+ph<=y+h);}
 bool crop(float &px,float &py,float &pw,float &ph)const {
  if(!enabled)return pw>0&&ph>0;
  float right=std::min(px+pw,x+w),bottom=std::min(py+ph,y+h);
  px=std::max(px,x);py=std::max(py,y);pw=right-px;ph=bottom-py;return pw>0&&ph>0;
 }
};
struct ScrollMotion {
 float x=0,y=0,start_x=0,start_y=0,press_x=0,press_y=0;
 bool active=false,dragged=false,bar_x=false,bar_y=false;
 void clamp(float max_x,float max_y){x=std::clamp(x,0.0f,std::max(0.0f,max_x));y=std::clamp(y,0.0f,std::max(0.0f,max_y));}
 void begin(float mx,float my,bool bx=false,bool by=false){active=true;dragged=false;press_x=mx;press_y=my;start_x=x;start_y=y;bar_x=bx;bar_y=by;}
 void move(float mx,float my,float max_x,float max_y,float vw,float vh,float threshold){
  if(!active)return;float dx=mx-press_x,dy=my-press_y;
  if(dx*dx+dy*dy>threshold*threshold)dragged=true;
  if(!dragged)return;
  if(bar_x)x=start_x+dx*max_x/std::max(1.0f,vw);else if(!bar_y)x=start_x-dx;
  if(bar_y)y=start_y+dy*max_y/std::max(1.0f,vh);else if(!bar_x)y=start_y-dy;
  clamp(max_x,max_y);
 }
};
}
