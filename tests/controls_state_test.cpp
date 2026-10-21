#include "controls_state.hpp"
#include <cassert>
using namespace lualoader::controls;
int main(){
 State s;assert(State::valid_id("bone_throw")&&!State::valid_id("a:b")&&!State::valid_id(""));
 auto k=State::key("mod.one","skill"),other=State::key("mod.two","skill");assert(k!=other);
 s.actions[k]={k,"mod.one","skill","投骨"};s.actions[other]={other,"mod.two","skill","冲刺"};
 s.sync({k});s.sync({k});assert(s.find(k)->presses==1&&s.find(k)->held&&!s.find(other)->held);
 s.sync({});assert(s.find(k)->releases==1&&!s.find(k)->held);
 s.find(k)->enabled=false;s.sync({k});assert(!s.find(k)->held&&s.find(k)->presses==1);
 s.bindings["custom1"]["slot1"]=k;s.begin_edit();s.snapshot();s.bindings["custom1"]["slot1"]=other;
 s.undo();assert(s.binding("custom1","slot1")==k);s.redo();assert(s.binding("custom1","slot1")==other);
 s.cancel();assert(s.binding("custom1","slot1")==k);
 s.bindings["custom2"]["slot1"]=other;s.erase("custom1","slot1");assert(s.binding("custom1","slot1").empty()&&s.binding("custom2","slot1")==other);
 s.remove_owner("mod.two");assert(!s.find(other)&&!s.binding("custom2","slot1").empty());
}
