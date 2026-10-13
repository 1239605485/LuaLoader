#pragma once
#include <string>
namespace lualoader::gui {
struct ClickState {
    std::string armed;
    bool consumed=false;
    bool update(const std::string &id,bool hover,bool pressed,bool released){
        if(pressed && hover && armed.empty())armed=id;
        if(released && hover && armed==id && !consumed){consumed=true;return true;}
        return false;
    }
    void finish(bool down,bool released){if(released||!down)armed.clear();consumed=false;}
    void reset(){armed.clear();consumed=false;}
};
}
