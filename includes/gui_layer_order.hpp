#pragma once
#include <algorithm>
namespace lualoader::gui::detail {
template<class Commands,class Boundary,class Draw>
void ordered_layers(Commands &commands,Boundary boundary,Draw draw){
    std::stable_sort(commands.begin(),commands.end(),[](const auto&a,const auto&b){return a.layer<b.layer;});
    int last=-1;
    for(const auto&command:commands){if(command.layer!=last){last=command.layer;boundary(last);}draw(command);}
}
}
