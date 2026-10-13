#pragma once
#include <cmath>
namespace lualoader::gui {
struct IconGesture {
    bool active=false,dragged=false;
    float press_x=0,press_y=0,origin_x=0,origin_y=0;
    struct Result {bool move=false,tap=false;float x=0,y=0;};
    Result update(float mx,float my,bool pressed,bool down,bool released,bool inside,
                  float x,float y,float threshold) {
        Result r;
        if(pressed && inside){active=true;dragged=false;press_x=mx;press_y=my;origin_x=x;origin_y=y;}
        if(active && (down || released)){
            const float dx=mx-press_x,dy=my-press_y;
            if(dx*dx+dy*dy>threshold*threshold)dragged=true;
            if(dragged){r.move=true;r.x=origin_x+dx;r.y=origin_y+dy;}
        }
        if(active && released){r.tap=inside && !dragged;active=false;dragged=false;}
        if(active && !down && !released && !pressed){active=false;dragged=false;}
        return r;
    }
};
}
