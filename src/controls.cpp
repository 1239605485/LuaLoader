// Native Android touch-layout bindings. All hooks use primitive/reference
// signatures; Vector2/Rectangle/Color are used only in fixed typed draw calls.
#include "controls.hpp"
#include "controls_state.hpp"
#include "logger.hpp"
#include "json.hpp"
#include <cmath>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <utility>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#if defined(__ANDROID__) && defined(__aarch64__)
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/patchlib/struct/string.h"
#include "tefkernel/patchlib/type.h"
#endif
namespace lualoader::controls {
namespace {
State state;
#if defined(__ANDROID__) && defined(__aarch64__)
void invalidate(const std::string& key,bool removing=false);
void refresh_modules();
#endif
std::filesystem::path storage;
bool ready=false;
std::string reason="native touch bindings require Android arm64";
void save(){
    if(storage.empty())return;
    try{
        nlohmann::json data={{"version",1},{"profiles",state.bindings}};
        auto temp=storage;temp+=".tmp";
        std::ofstream file(temp,std::ios::binary|std::ios::trunc);
        if(!file)throw std::runtime_error("cannot open binding configuration");
        file<<data.dump(2);file.flush();if(!file)throw std::runtime_error("binding configuration write failed");file.close();
        std::filesystem::rename(temp,storage);
    }catch(const std::exception& e){LOG_WARN("[controls] save failed: {}",e.what());}
}
lua_mod_handle_t* owner(lua_State* L){return static_cast<lua_mod_handle_t*>(lua_touserdata(L,lua_upvalueindex(1)));}
std::string own_key(lua_State* L,int arg=1){return State::key(owner(L)->mod_id,luaL_checkstring(L,arg));}
int available(lua_State* L){lua_pushboolean(L,ready);lua_pushstring(L,reason.c_str());return 2;}
int add(lua_State* L){
    if(!ready)return luaL_error(L,"native touch bindings unavailable: %s",reason.c_str());
    luaL_checktype(L,1,LUA_TTABLE);
    lua_getfield(L,1,"id");std::string id=luaL_checkstring(L,-1);lua_pop(L,1);
    if(!State::valid_id(id))return luaL_error(L,"control id must be 1-64 ASCII letters, digits, _, - or .");
    lua_getfield(L,1,"name");std::string name=luaL_checkstring(L,-1);lua_pop(L,1);
    if(name.empty()||name.size()>128)return luaL_error(L,"control name must be 1-128 UTF-8 bytes");
    auto h=owner(L);auto key=State::key(h->mod_id,id);
    if(state.actions.count(key))return luaL_error(L,"control already registered: %s",id.c_str());
    if(state.actions.size()>=256)return luaL_error(L,"native control registry is full");
    state.actions.emplace(key,Action{key,h->mod_id,id,name});
#if defined(__ANDROID__) && defined(__aarch64__)
    refresh_modules();
#endif
    lua_pushlstring(L,id.data(),id.size());return 1;
}
int remove(lua_State* L){auto key=own_key(L);
#if defined(__ANDROID__) && defined(__aarch64__)
    invalidate(key,true);
#endif
    const bool removed=state.actions.erase(key)!=0;
#if defined(__ANDROID__) && defined(__aarch64__)
    if(removed)refresh_modules();
#endif
    lua_pushboolean(L,removed);return 1;}

int down(lua_State* L){auto a=state.find(own_key(L));lua_pushboolean(L,a&&a->held&&a->enabled&&a->visible);return 1;}
int edge(lua_State* L,bool press){auto a=state.find(own_key(L));bool hit=false;
    if(a&&a->enabled&&a->visible){auto& n=press?a->presses:a->releases;hit=n>0;if(hit)--n;}
    lua_pushboolean(L,hit);return 1;
}
int pressed(lua_State* L){return edge(L,true);}int released(lua_State* L){return edge(L,false);}
int flag(lua_State* L,bool visible){auto a=state.find(own_key(L));if(!a)return luaL_error(L,"control not registered");
    const bool value=lua_toboolean(L,2);(visible?a->visible:a->enabled)=value;
    if(!value){
#if defined(__ANDROID__) && defined(__aarch64__)
        invalidate(a->key);
#endif
        a->held=false;a->presses=0;a->releases=0;}return 0;
}
int enable(lua_State* L){return flag(L,false);}int show(lua_State* L){return flag(L,true);}
#if defined(__ANDROID__) && defined(__aarch64__)
using H=patch_handle_t;
struct Vec{float x,y;};struct Rect{int x=0,y=0,w=0,h=0;};struct Color{unsigned char a,b,g,r;};
static_assert(sizeof(Vec)==8&&sizeof(Rect)==16&&sizeof(Color)==4);
struct Api {
 H main{},styles{},binding{},slot{},slot_type{},panel{},setting{},quick{},editor{},interface_edit{},cursor{},manager{},gui{};
 H batch{},width{},height{},menu{},menu_getter{},styles_instance{},active_profile{},profile_id{},profile_controls{},binding_slot{},binding_action{};
 H slot_id{},slot_style{},slot_kind{},slot_multi{},active_control{},active_slot{},action_binding{},display_options{};
 H gui_active{},gui_edit{},edit_enabled{},gui_editor{},gui_runtime{},ui_open{};
 H manager_instance{},cursor_id{},cursor_pos{},cursor_down{},cursor_old{},cursor_ignore{},cursor_world{};
 H rect{},text{},get_region{},adjust{},count{},at{},update_value{},close_options{},is_over_options{},add_undo{},has_undo{},has_redo{};
} api;
std::vector<patch_hook_id_t> hooks;
unsigned long long tick=0;
constexpr int unbound=28; // supplied PE 1.4.5.8 ContolType.Count
constexpr int carrier=27; // safe, existing ArmorSetAbility, never persisted for mods
struct Region{Rect rect;std::string key,profile,slot;unsigned long long tick=0;bool multi=true;};
std::map<std::string,Region> regions;
struct Choice{int kind=0,value=0;std::string key;Rect rect;unsigned epoch=0;};
struct Claim{bool popup=false,cancelled=false,released=false;Region region;Choice choice;};
std::map<int,Claim> claims;
std::vector<Choice> rows;
Rect popup_rect;
unsigned popup_epoch=0;
unsigned long long popup_tick=0;
int popup_page=0;
std::vector<std::string> module_ids;
int popup_module=0;
H popup_owner=nullptr;
std::string popup_context;
std::optional<Choice> selected;
struct DrawContext{std::string key,slot;bool multi=true;};
std::vector<DrawContext> drawing;
unsigned runtime_depth=0;
std::set<H> render_bindings;
bool history_transition=false;
bool selecting=false;
std::vector<std::pair<std::string,std::string>> changes;
void invalidate(const std::string& key,bool removing){
    for(auto& [id,claim]:claims)if((!claim.popup&&claim.region.key==key)||(removing&&claim.popup&&claim.choice.key==key))claim.cancelled=true;
    if(removing&&selected&&selected->key==key)selected.reset();
    for(auto it=regions.begin();it!=regions.end();)if(it->second.key==key)it=regions.erase(it);else ++it;
}
H need(H h,const char* name){if(!h||!patchlib_is_valid(h))throw std::runtime_error(std::string("missing ")+name);return h;}
H type(const char* name,const char* ns=""){return need(patchlib_type_get_type(ns,name),name);}
H field(H t,const char* name){return need(patchlib_type_get_field(t,name),name);}
H method(H t,const char* name,int n){return need(patchlib_type_get_method_by_param_count(t,name,n),name);}
template<class T>T read(H f,H object=nullptr){T v{};if(f){auto ptr=patchlib_field_get_pointer(f,object);if(ptr)std::memcpy(&v,ptr,sizeof(v));}return v;}
template<class T>void write(H f,H object,T value){patchlib_field_set_value(f,object,&value);}
template<class T>T invoke(H m,H obj=nullptr,void** args=nullptr){
    // Android's kernel passes this buffer directly to ffi_call. Small integer
    // returns are widened to ffi_arg (one machine word), including bool/int.
    alignas(std::uintptr_t) unsigned char result[std::max(sizeof(T),sizeof(std::uintptr_t))]{};
    if(!patchlib_method_invoke_args(m,obj,result,args))throw std::runtime_error("native control call failed");
    T out{};std::memcpy(&out,result,sizeof(out));return out;
}
void call(H m,H obj=nullptr,void** args=nullptr){if(!patchlib_method_invoke_args(m,obj,nullptr,args))throw std::runtime_error("native control call failed");}
std::string string(H s){if(!s)return {};char* p=patchlib_string_cstr(s);if(!p)return {};std::string out(p);std::free(p);return out;}
H profile(){auto s=read<H>(api.styles_instance);return s?invoke<H>(api.active_profile,s):nullptr;}
std::string profile_key(){auto p=profile();return p?string(read<H>(api.profile_id,p)):std::string{};}
H gui_object(){return read<H>(api.gui_active);}
H edit_object(){auto g=gui_object();return g?read<H>(api.gui_edit,g):nullptr;}
bool in_menu(){return api.menu?read<bool>(api.menu):invoke<bool>(api.menu_getter);}
bool editing(){auto edit=edit_object();return edit&&read<bool>(api.edit_enabled,edit);}
bool ui_blocked(){auto g=gui_object();auto c=g?read<H>(api.gui_runtime,g):nullptr;return !c||invoke<bool>(api.ui_open,c);}
H native_binding(const std::string& slot){auto p=profile();if(!p)return nullptr;auto arr=read<H>(api.profile_controls,p);if(!arr)return nullptr;
    for(size_t i=0,n=std::min<size_t>(patchlib_array_length(arr),2048);i<n;++i){H b=nullptr;patchlib_array_at(arr,i,&b);if(b&&string(read<H>(api.binding_slot,b))==slot)return b;}return nullptr;
}
std::string custom_binding(H b){if(!b)return {};int action=read<int>(api.binding_action,b);
    if(action!=unbound&&!(action==carrier&&render_bindings.count(b)))return {};
    return state.binding(profile_key(),string(read<H>(api.binding_slot,b)));
}
std::string custom_slot(H slot){if(!slot)return {};auto style=read<H>(api.slot_style,slot);
    if(!style||read<int>(api.slot_kind,style)!=1)return {};
    return custom_binding(native_binding(string(read<H>(api.slot_id,slot))));
}
std::string active_key(){return custom_binding(read<H>(api.active_control));}
std::string active_slot(){auto b=read<H>(api.active_control);return b?string(read<H>(api.binding_slot,b)):std::string{};}
bool contains(Rect r,float x,float y){return r.w>0&&r.h>0&&x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;}
Color color(int r,int g,int b,int a=255){return {static_cast<unsigned char>(a),static_cast<unsigned char>(b),static_cast<unsigned char>(g),static_cast<unsigned char>(r)};}
void rectangle(Rect r,Color c){auto sb=read<H>(api.batch);if(!sb||r.w<=0||r.h<=0)return;
    using Fn=void(*)(void*,Rect,Color);auto fn=reinterpret_cast<Fn>(patchlib_method_get_pointer(api.rect));if(fn)fn(sb,r,c);
}
void inset(Rect r,int amount){r.x+=amount;r.y+=amount;r.w-=amount*2;r.h-=amount*2;}
void frame(Rect r,Color edge,Color fill,Color light,Color shade){
    rectangle(r,edge);inset(r,2);rectangle(r,fill);
    rectangle({r.x,r.y,r.w,2},light);
    rectangle({r.x,r.y+r.h-2,r.w,2},shade);
}
void text(const std::string& value,Rect r,Color c,bool center=false){auto sb=read<H>(api.batch);if(!sb||value.empty())return;
    size_t chars=0;for(unsigned char ch:value)if((ch&0xc0)!=0x80)++chars;
    float scale=std::min(0.85f,(r.w-12)/std::max(1.0f,chars*26.0f));
    scale=std::max(0.15f,scale);float x=static_cast<float>(r.x+8);if(center)x=r.x+r.w/2.0f-chars*26.0f*scale/2;
    auto s=patchlib_string_create(value.c_str());if(!s)return;
    using Fn=Vec(*)(void*,void*,Vec,Color,float,float,float,int);
    auto fn=reinterpret_cast<Fn>(patchlib_method_get_pointer(api.text));if(fn)fn(sb,s,{x,r.y+(r.h-28*scale)/2},c,scale,0,0,-1);
    patchlib_free(s);
}
Rect region(H layout){using Fn=Rect(*)(void*);auto fn=reinterpret_cast<Fn>(patchlib_method_get_pointer(api.get_region));return fn&&layout?fn(layout):Rect{};}
void clear_input(){claims.clear();regions.clear();state.reset_input();}
std::string module_title(const std::string& id){
    std::vector<std::string> parts;size_t start=0;
    while(start<=id.size()){
        const size_t end=id.find('.',start);
        parts.push_back(id.substr(start,end==std::string::npos?std::string::npos:end-start));
        if(end==std::string::npos)break;
        start=end+1;
    }
    if(parts.size()>=3)parts.erase(parts.begin());
    if(parts.size()>1){
        auto tail=parts.back();std::transform(tail.begin(),tail.end(),tail.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(tail=="probe"||tail=="diagnostic"||tail=="debug"||tail=="test")parts.pop_back();
    }
    std::string label;
    for(const auto& part:parts){
        if(!label.empty())label+=' ';
        bool capitalize=true;
        for(unsigned char c:part){
            if(c=='_'||c=='-'){label+=' ';capitalize=true;continue;}
            label+=capitalize?static_cast<char>(std::toupper(c)):static_cast<char>(c);
            capitalize=false;
        }
    }
    return label.empty()?id:label;
}
void refresh_modules(){
    const std::string current=module_ids.empty()||popup_module<0||static_cast<size_t>(popup_module)>=module_ids.size()?std::string{}:module_ids[popup_module];
    std::set<std::string> owners;for(const auto& [key,a]:state.actions)owners.insert(a.owner);
    module_ids.assign(owners.begin(),owners.end());
    auto it=std::find(module_ids.begin(),module_ids.end(),current);
    popup_module=it==module_ids.end()?0:static_cast<int>(it-module_ids.begin());
    if(module_ids.empty())popup_page=0;
}
int module_action_count(const std::string& owner){
    int count=0;for(const auto& [key,a]:state.actions)if(a.owner==owner)++count;return count;
}
void popup_closed(H obj,void**,void*,const patch_method_signature_t*){
    if(obj!=popup_owner)return;
    rows.clear();selected.reset();popup_owner=nullptr;popup_context.clear();popup_tick=0;++popup_epoch;
}
void close_popup(H obj){call(api.close_options,obj);rows.clear();selected.reset();popup_owner=nullptr;popup_context.clear();++popup_epoch;}
void choose(H obj,Choice choice){
    if(choice.kind==3){
        if(module_ids.empty())return;
        const int count=module_action_count(module_ids[popup_module]);
        const int pages=std::max(1,(count+3)/4);
        popup_page=std::clamp(popup_page+choice.value,0,pages-1);++popup_epoch;return;
    }
    if(choice.kind==4){
        if(module_ids.size()<2)return;
        const int count=static_cast<int>(module_ids.size());
        popup_module=(popup_module+choice.value+count)%count;popup_page=0;++popup_epoch;return;
    }
    if(choice.kind!=2||!state.find(choice.key))return;
    auto slot=active_slot(),p=profile_key();if(slot.empty()||p.empty())return;
    auto edit=edit_object();if(edit)call(api.add_undo,edit); // participates in native undo/redo
    int value=unbound;void* args[]={&value};
    selecting=true;
    try{call(api.update_value,obj,args);}catch(...){selecting=false;throw;}
    selecting=false;
    state.bindings[p][slot]=choice.key;
    close_popup(obj);
}
bool popup_hover_prefix(H obj,void** args,const patch_method_signature_t*,void* result){
    if(!result||obj!=popup_owner||!read<bool>(api.display_options,obj)||tick-popup_tick>2||!args||!args[0])return false;
    const Vec position=*static_cast<const Vec*>(args[0]);
    if(!contains(popup_rect,position.x,position.y))return false;
    *static_cast<bool*>(result)=true;return true;
}
bool picker_prefix(H obj,void**,const patch_method_signature_t*,void*){
    // Always let the game's overlay draw and process its own vanilla options.
    if(obj==popup_owner&&selected){
        auto choice=*selected;selected.reset();
        if(choice.epoch==popup_epoch){choose(obj,choice);if(!popup_owner)return true;}
    }
    return false;
}
void picker_postfix(H obj,void**,void*,const patch_method_signature_t*){
    if(state.actions.empty()||!read<bool>(api.display_options,obj)||read<int>(api.action_binding,obj)!=1){
        if(obj==popup_owner)popup_closed(obj,nullptr,nullptr,nullptr);
        return;
    }
    auto config=read<H>(api.active_slot),style=config?read<H>(api.slot_style,config):nullptr;
    if(!style||read<int>(api.slot_kind,style)!=1)return;
    const int width=invoke<int>(api.width),height=invoke<int>(api.height);
    if(width<300||height<160||module_ids.empty())return;
    const std::string context=profile_key()+"\n"+active_slot();
    if(obj!=popup_owner||context!=popup_context){popup_owner=obj;popup_context=context;popup_page=0;popup_module=0;selected.reset();++popup_epoch;}
    popup_module=std::clamp(popup_module,0,static_cast<int>(module_ids.size())-1);
    const std::string& selected_module=module_ids[popup_module];
    const int action_count=module_action_count(selected_module),pages=std::max(1,(action_count+3)/4);
    popup_page=std::clamp(popup_page,0,pages-1);
    const int width_px=std::min(width-16,std::max(420,width*50/100));
    const int x=(width-width_px)/2,row_h=std::clamp(height/19,30,40),header_h=30,gap=6;
    const int first_action=popup_page*4;
    const int dock_h=8+header_h+gap+row_h*2+8;
    const int y=std::max(8,height-dock_h-8);
    popup_rect={x,y,width_px,dock_h};
    popup_tick=tick;
    rows.clear();
    // Deep navy, cobalt frame and bright blue cards follow Terraria's pixel UI palette.
    frame(popup_rect,color(5,9,24,248),color(20,31,70,248),color(91,112,199),color(8,14,37));
    Rect header{x+7,y+6,width_px-14,header_h};
    frame(header,color(8,14,34),color(27,42,89),color(83,105,185),color(13,23,53));
    const int module_count=static_cast<int>(module_ids.size());
    const int header_y=header.y+3,header_button=header_h-6;
    auto header_button_draw=[&](Rect r,const std::string& label,bool enabled){
        frame(r,enabled?color(8,16,39):color(13,22,47),enabled?color(45,72,126):color(31,44,75),
              enabled?color(102,128,209):color(57,69,103),color(20,34,66));
        text(label,r,enabled?color(235,241,255):color(112,126,157),true);
    };
    const bool can_switch=module_count>1;
    Rect module_prev{x+10,header_y,header_button,header_button};
    header_button_draw(module_prev,"‹",can_switch);
    Choice module_prev_choice{can_switch?4:0,-1,{},module_prev,popup_epoch};
    if(can_switch)rows.push_back(module_prev_choice);
    const int module_next_x=x+width_px*53/100;
    Rect module_title_rect{x+module_prev.w+18,header.y+2,module_next_x-(x+module_prev.w+24),header_h-4};
    text(module_title(selected_module),module_title_rect,color(244,247,255),true);
    Rect module_next{module_next_x,header_y,header_button,header_button};
    header_button_draw(module_next,"›",can_switch);
    Choice module_next_choice{can_switch?4:0,1,{},module_next,popup_epoch};
    if(can_switch)rows.push_back(module_next_choice);
    text(std::to_string(action_count)+" 项",{x+width_px*59/100,header.y+2,width_px*13/100,header_h-4},color(183,198,239),true);
    const int page_prev_x=x+width_px*73/100;
    Rect page_prev{page_prev_x,header_y,header_button,header_button};
    const bool has_prev=popup_page>0,has_next=popup_page+1<pages;
    header_button_draw(page_prev,"‹",has_prev);
    Choice page_prev_choice{has_prev?3:0,-1,{},page_prev,popup_epoch};
    if(has_prev)rows.push_back(page_prev_choice);
    Rect page_label{x+width_px*78/100,header.y+2,width_px*12/100,header_h-4};
    text(std::to_string(popup_page+1)+"/"+std::to_string(pages),page_label,color(235,241,255),true);
    Rect page_next{x+width_px*91/100,header_y,header_button,header_button};
    header_button_draw(page_next,"›",has_next);
    Choice page_next_choice{has_next?3:0,1,{},page_next,popup_epoch};
    if(has_next)rows.push_back(page_next_choice);
    int index=0;
    for(auto& [key,a]:state.actions){
        if(a.owner!=selected_module)continue;
        const int local=index++-first_action;
        if(local<0||local>=4)continue;
        const int col_w=(width_px-22)/2;
        Rect r{x+7+(local%2)*(col_w+8),header.y+header_h+gap+(local/2)*row_h,col_w,row_h-3};
        Choice choice{2,0,key,r,popup_epoch};
        frame(r,color(7,15,37),color(48,78,137),color(115,140,220),color(25,43,83));
        text(a.name,{r.x+10,r.y+1,r.w-20,r.h-2},color(249,250,255),true);
        rows.push_back(choice);
    }
}

bool value_prefix(H obj,void**,const patch_method_signature_t*,void* result){
    if(!result||read<int>(api.action_binding,obj)!=1)return false;
    auto a=state.find(active_key());if(!a)return false;
    *static_cast<H*>(result)=patchlib_string_create(a->name.c_str());return true;
}
bool visible_prefix(H,void** args,const patch_method_signature_t*,void* result){
    if(!args||!args[2]||!result)return false;
    auto a=state.find(custom_slot(*static_cast<H*>(args[2])));if(!a)return false;
    *static_cast<bool*>(result)=a->visible&&!ui_blocked();return true;
}
bool slot_prefix(H,void** args,const patch_method_signature_t*,void*){
    H slot=args&&args[1]?*static_cast<H*>(args[1]):nullptr;
    drawing.push_back({custom_slot(slot),slot?string(read<H>(api.slot_id,slot)):std::string{},slot&&read<bool>(api.slot_multi,slot)});return false;
}
void slot_postfix(H,void**,void*,const patch_method_signature_t*){if(!drawing.empty())drawing.pop_back();}
bool quick_prefix(H,void** args,const patch_method_signature_t*,void*){
    if(drawing.empty()||!args)return false;
    auto a=state.find(drawing.back().key);if(!a)return false;
    if(args[2])*static_cast<bool*>(args[2])=a->held;
    if(args[4])*static_cast<bool*>(args[4])=true;
    if(args[5])*static_cast<bool*>(args[5])=true;
    return false;
}
void quick_postfix(H,void** args,void*,const patch_method_signature_t*){
    if(drawing.empty()||!args||!args[1])return;
    auto a=state.find(drawing.back().key);if(!a||!a->visible)return;
    Rect r=region(*static_cast<H*>(args[1]));if(r.w<=0||r.h<=0)return;
    text(a->name,r,a->enabled?color(245,247,255):color(130,135,150),true);
    auto p=profile_key();regions[p+"\n"+drawing.back().slot]={r,a->key,p,drawing.back().slot,tick,drawing.back().multi};
}
void restore_render(){for(auto b:render_bindings)write(api.binding_action,b,unbound);render_bindings.clear();}
bool runtime_prefix(H,void**,const patch_method_signature_t*,void*){
    ++runtime_depth;if(runtime_depth!=1)return false;
    auto p=profile();auto arr=p?read<H>(api.profile_controls,p):nullptr;
    for(size_t i=0,n=arr?std::min<size_t>(patchlib_array_length(arr),2048):0;i<n;++i){H b=nullptr;patchlib_array_at(arr,i,&b);
        auto a=state.find(custom_binding(b));if(a&&a->visible&&!ui_blocked()){render_bindings.insert(b);write(api.binding_action,b,carrier);}}
    return false;
}
void runtime_postfix(H,void**,void*,const patch_method_signature_t*){if(runtime_depth)--runtime_depth;if(!runtime_depth){restore_render();drawing.clear();}}
void tick_postfix(H,void**,void*,const patch_method_signature_t*){++tick;
    for(auto i=regions.begin();i!=regions.end();)if(tick-i->second.tick>2)i=regions.erase(i);else ++i;
    if(in_menu()||editing())state.sync({});
}
void poll_postfix(H,void**,void*,const patch_method_signature_t*){
    if(in_menu()){clear_input();return;}
    auto cm=read<H>(api.manager_instance);if(!cm){clear_input();return;}
    bool in_editor=editing();bool picker=in_editor&&popup_owner&&tick-popup_tick<=2;
    bool blocked=!in_editor&&ui_blocked();
    if(blocked){state.reset_input();for(auto& [id,c]:claims)if(!c.popup)c.cancelled=true;}
    auto p=profile_key();std::set<int> live;std::set<std::string> held;
    for(auto i=claims.begin();i!=claims.end();)if(i->second.released)i=claims.erase(i);else ++i;
    int count=invoke<int>(api.count,cm);
    using Adjust=Vec(*)(Vec);auto adjust=reinterpret_cast<Adjust>(patchlib_method_get_pointer(api.adjust));
    for(int index=0;index<std::clamp(count,0,32);++index){void* arg[]={&index};H cur=invoke<H>(api.at,cm,arg);if(!cur)continue;
        int id=read<int>(api.cursor_id,cur);live.insert(id);Vec pos=read<Vec>(api.cursor_pos,cur);pos=adjust(pos);
        bool down=read<bool>(api.cursor_down,cur),old=read<bool>(api.cursor_old,cur);
        auto claim=claims.find(id);
        if(claim==claims.end()&&down&&!old){
            if(picker&&contains(popup_rect,pos.x,pos.y)){
                Claim v;v.popup=true;v.choice={0,0,{},popup_rect,popup_epoch};for(auto& row:rows)if(contains(row.rect,pos.x,pos.y)){v.choice=row;break;}
                claim=claims.emplace(id,v).first;
            }else if(!in_editor&&!blocked&&!read<bool>(api.cursor_ignore,cur)){for(auto& [key,r]:regions){auto a=state.find(r.key);if(r.profile==p&&tick-r.tick<=2&&a&&a->visible&&contains(r.rect,pos.x,pos.y)){
                Claim v;v.region=r;
                if(!r.multi)for(auto& [other_id,other]:claims)if(!other.popup&&!other.released&&!other.cancelled&&other.region.profile==r.profile&&other.region.slot==r.slot)v.cancelled=true;
                claim=claims.emplace(id,v).first;break;}}
            }
        }
        if(claim==claims.end())continue;
        auto& v=claim->second;write(api.cursor_ignore,cur,true);write(api.cursor_world,cur,true);
        Rect r=v.popup?v.choice.rect:v.region.rect;
        if(!contains(r,pos.x,pos.y))v.cancelled=true;
        if(v.popup){if(!down&&!v.cancelled&&picker&&v.choice.epoch==popup_epoch&&v.choice.kind)selected=v.choice;}
        else if(down&&!v.cancelled&&!in_editor&&!blocked&&v.region.profile==p)held.insert(v.region.key);
        if(!down)v.released=true;
    }
    for(auto i=claims.begin();i!=claims.end();)if(!live.count(i->first))i=claims.erase(i);else ++i;
    state.sync(held);
}
bool dispatch_prefix(H,void** args,const patch_method_signature_t*,void*){
    return args&&args[0]&&claims.count(read<int>(api.cursor_id,*static_cast<H*>(args[0])));
}
bool update_prefix(H obj,void**,const patch_method_signature_t*,void*){
    changes.emplace_back(!selecting&&read<int>(api.action_binding,obj)==1?profile_key():std::string{},active_slot());
    return false;
}
void change_postfix(H,void**,void*,const patch_method_signature_t*){
    if(changes.empty())return;
    auto change=changes.back();changes.pop_back();
    if(!change.first.empty())state.erase(change.first,change.second);
}
bool remove_slot_prefix(H,void** args,const patch_method_signature_t*,void*){
    std::string slot;
    if(args&&args[0]){int index=*static_cast<int*>(args[0]);auto p=profile();auto array=p?read<H>(api.profile_controls,p):nullptr;
        if(array&&index>=0&&static_cast<size_t>(index)<patchlib_array_length(array)){H b=nullptr;patchlib_array_at(array,index,&b);if(b)slot=string(read<H>(api.binding_slot,b));}}
    changes.emplace_back(slot.empty()?std::string{}:profile_key(),slot);return false;
}
void undo_snapshot(H,void**,void*,const patch_method_signature_t*){if(!history_transition)state.snapshot();}
bool undo_prefix(H obj,void**,const patch_method_signature_t*,void*){history_transition=true;if(invoke<bool>(api.has_undo,obj))state.undo();return false;}
bool redo_prefix(H obj,void**,const patch_method_signature_t*,void*){history_transition=true;if(invoke<bool>(api.has_redo,obj))state.redo();return false;}
void history_done(H,void**,void*,const patch_method_signature_t*){history_transition=false;}
void redo_clear(H,void**,void*,const patch_method_signature_t*){if(!history_transition)state.redos.clear();}
void backup_postfix(H,void**,void*,const patch_method_signature_t*){state.begin_edit();clear_input();}
void cancel_postfix(H,void**,void*,const patch_method_signature_t*){state.cancel();clear_input();}
void history_postfix(H,void**,void*,const patch_method_signature_t*){state.clear_history();}
bool save_prefix(H,void**,const patch_method_signature_t*,void*){restore_render();return false;}
void save_postfix(H,void**,void*,const patch_method_signature_t*){save();if(state.has_backup)state.backup=state.bindings;}
void reset_postfix(H,void**,void*,const patch_method_signature_t*){state.bindings.clear();state.clear_history();clear_input();save();}
void copy_postfix(H,void**,void*,const patch_method_signature_t*){auto i=state.bindings.find(profile_key());state.clipboard=i==state.bindings.end()?Slots{}:i->second;state.has_clipboard=true;}
void paste_postfix(H,void**,void*,const patch_method_signature_t*){if(state.has_clipboard)state.bindings[profile_key()]=state.clipboard;clear_input();}
void default_postfix(H,void**,void*,const patch_method_signature_t*){state.bindings.erase(profile_key());clear_input();}
struct Callback{prefix_callback_t pre=nullptr;postfix_callback_t post=nullptr;bool finalize=false;};
std::array<Callback,32> callbacks;
void stop(const char* message){if(ready)LOG_WARN("[controls] disabled after native callback failure: {}",message);ready=false;reason=message;state.reset_input();}
template<size_t I>bool pre_guard(H obj,void** args,const patch_method_signature_t* sig,void* result){
    if(!ready)return false;
    try{return callbacks[I].pre?callbacks[I].pre(obj,args,sig,result):false;}
    catch(const std::exception& e){stop(e.what());}catch(...){stop("unknown native binding failure");}return false;
}
template<size_t I>void post_guard(H obj,void** args,void* result,const patch_method_signature_t* sig){
    if(!ready&&!callbacks[I].finalize)return;
    try{if(callbacks[I].post)callbacks[I].post(obj,args,result,sig);}
    catch(const std::exception& e){stop(e.what());}catch(...){stop("unknown native binding failure");}
}
template<size_t... I>auto prefix_table(std::index_sequence<I...>){return std::array<prefix_callback_t,sizeof...(I)>{pre_guard<I>...};}
template<size_t... I>auto postfix_table(std::index_sequence<I...>){return std::array<postfix_callback_t,sizeof...(I)>{post_guard<I>...};}
void add_hook(H target,prefix_callback_t pre,postfix_callback_t post,bool finalize=false){
    static const auto pres=prefix_table(std::make_index_sequence<32>{});static const auto posts=postfix_table(std::make_index_sequence<32>{});
    const auto index=hooks.size();if(index>=callbacks.size())throw std::runtime_error("native control hook capacity exceeded");
    callbacks[index]={pre,post,finalize};
    auto h=patchlib_install_prepost_hook(target,pre?pres[index]:nullptr,post?posts[index]:nullptr);
    if(h==PATCH_HOOK_INVALID_ID)throw std::runtime_error("native control hook installation failed");
    hooks.push_back(h);
}
void resolve(){
    api.main=type("Main","Terraria");api.styles=type("VirtualControlsStyles_Layout");api.binding=type("VirtualControlsBinding_Layout");
    api.slot=type("VirtualControlsHardwareConfigurationMapping_Layout");api.slot_type=type("VirtualControlsSlotTypeMapping_Layout");
    api.panel=type("GUIVirtualInputControllerEdit_ControlPanel");api.setting=need(patchlib_type_get_inner_type(api.panel,"ControlPanelActionSetting"),"ControlPanelActionSetting");
    api.quick=type("GUIQuickActionButton");api.editor=type("GUIVirtualInputControllerEdit");api.interface_edit=type("GUIInterfaceEdit");
    api.gui=type("GUIInstance");api.cursor=type("Cursor");api.manager=type("CursorManager");
    api.batch=field(api.main,"spriteBatch");api.width=method(api.main,"get_screenWidth",0);api.height=method(api.main,"get_screenHeight",0);api.menu=patchlib_type_get_field(api.main,"gameMenu");if(!api.menu||!patchlib_is_valid(api.menu)){api.menu=nullptr;api.menu_getter=method(api.main,"get_gameMenu",0);}
    api.styles_instance=field(api.styles,"Instance");api.active_profile=method(api.styles,"get_ActiveProfile",0);
    auto prof=type("VirtualControlsProfile_Layout");api.profile_id=field(prof,"Id");api.profile_controls=field(prof,"Controls");
    api.binding_slot=field(api.binding,"SlotId");api.binding_action=field(api.binding,"Action");api.slot_id=field(api.slot,"SlotId");api.slot_multi=field(api.slot,"MultiTouch");api.slot_style=field(api.slot,"Style");api.slot_kind=field(api.slot_type,"ControlType");
    api.active_control=field(api.panel,"ActiveControl");api.active_slot=field(api.panel,"ActiveSlotConfig");api.action_binding=field(api.setting,"ActionBinding");
    api.display_options=field(type("GUIPulldownSetting"),"displayOptions");api.gui_active=field(api.gui,"Active");api.gui_edit=field(api.gui,"GUIInterfaceEdit");api.gui_editor=field(api.gui,"GUIVirtualInputControllerEdit");api.edit_enabled=field(api.interface_edit,"Enabled");api.gui_runtime=field(api.gui,"GUIVirtualInputController");api.ui_open=method(type("GUIVirtualInputController"),"get_InGameUIOpen",0);
    api.manager_instance=field(api.manager,"Instance");api.cursor_id=field(api.cursor,"Id");api.cursor_pos=field(api.cursor,"Position");api.cursor_down=field(api.cursor,"Down");api.cursor_old=field(api.cursor,"WasDown");api.cursor_ignore=field(api.cursor,"Ignore");api.cursor_world=field(api.cursor,"IgnoreForWorld");
    api.rect=method(type("Utils","Terraria"),"DrawInvBG",3);api.text=method(type("Utils","Terraria"),"DrawBorderString",8);api.get_region=method(api.quick,"GetRegionWithoutPicking",1);
    api.adjust=method(type("XNAUnityRunner"),"AdjustRealScreenForUI",1);api.count=method(api.manager,"GetNumCursors",0);api.at=method(api.manager,"GetCursor",1);
    api.update_value=method(api.setting,"UpdateValue",1);api.close_options=method(type("GUIPulldownSetting"),"CloseOptions",0);api.is_over_options=method(type("GUIPulldownSetting"),"IsOverOptions",1);
    api.add_undo=method(api.interface_edit,"AddControlUndo",0);api.has_undo=method(api.interface_edit,"HasUndo",0);api.has_redo=method(api.interface_edit,"HasRedo",0);
}
#endif
}
void configure_storage(const std::string& directory){
    state.bindings.clear();state.backup.clear();state.clipboard.clear();state.has_backup=false;state.has_clipboard=false;state.clear_history();
    storage=std::filesystem::path(directory)/"controls_bindings.json";
    try{std::ifstream file(storage);if(!file)return;nlohmann::json j;file>>j;if(j.value("version",0)!=1)return;
        auto p=j.at("profiles").get<Bindings>();if(p.size()>128)throw std::runtime_error("too many control profiles");
        for(auto& [profile,slots]:p){if(profile.empty()||profile.size()>512||slots.size()>2048)throw std::runtime_error("invalid profile bindings");
            for(auto& [slot,key]:slots)if(slot.empty()||slot.size()>512||key.size()>512)throw std::runtime_error("invalid slot binding");}
        state.bindings=std::move(p);
    }catch(const std::exception& e){LOG_WARN("[controls] ignored invalid binding configuration: {}",e.what());}
}
void install(){
#if defined(__ANDROID__) && defined(__aarch64__)
    if(ready)return;
    try{
        // TPF leaves unsupported exports null; C++ catch cannot catch SIGSEGV.
#define REQUIRE_CONTROL_API(name) if (!name) throw std::runtime_error("kernel export missing: " #name)
        REQUIRE_CONTROL_API(patchlib_is_valid);
        REQUIRE_CONTROL_API(patchlib_type_get_type);
        REQUIRE_CONTROL_API(patchlib_type_get_field);
        REQUIRE_CONTROL_API(patchlib_type_get_method_by_param_count);
        REQUIRE_CONTROL_API(patchlib_type_get_inner_type);
        REQUIRE_CONTROL_API(patchlib_field_get_pointer);
        REQUIRE_CONTROL_API(patchlib_field_set_value);
        REQUIRE_CONTROL_API(patchlib_method_invoke_args);
        REQUIRE_CONTROL_API(patchlib_method_get_pointer);
        REQUIRE_CONTROL_API(patchlib_string_cstr);
        REQUIRE_CONTROL_API(patchlib_string_create);
        REQUIRE_CONTROL_API(patchlib_array_length);
        REQUIRE_CONTROL_API(patchlib_array_at);
        REQUIRE_CONTROL_API(patchlib_install_prepost_hook);
        REQUIRE_CONTROL_API(patchlib_uninstall_hook);
#undef REQUIRE_CONTROL_API
        resolve();
        add_hook(method(api.setting,"DrawOverlay",0),picker_prefix,picker_postfix);
        add_hook(api.is_over_options,popup_hover_prefix,nullptr);
        add_hook(method(api.setting,"GetValue",0),value_prefix,nullptr);
        add_hook(api.update_value,update_prefix,change_postfix,true);
        add_hook(api.close_options,nullptr,popup_closed);
        add_hook(method(api.editor,"RemoveSlot",1),remove_slot_prefix,change_postfix,true);
        auto controller=type("GUIVirtualInputController");
        add_hook(method(controller,"Draw",0),runtime_prefix,runtime_postfix,true);
        add_hook(method(controller,"UpdateControlVisibilityRegions",1),runtime_prefix,runtime_postfix,true);
        add_hook(method(controller,"DetermineButtonVisible",3),visible_prefix,nullptr);
        add_hook(method(controller,"DrawButtonControl",4),slot_prefix,slot_postfix);
        add_hook(method(api.quick,"Draw",6),quick_prefix,quick_postfix);
        add_hook(method(api.manager,"UpdateCursorList",0),nullptr,poll_postfix);
        add_hook(method(type("CursorInputLayer"),"UpdateCursor",1),dispatch_prefix,nullptr);
        add_hook(method(api.main,"Update",1),nullptr,tick_postfix);
        add_hook(api.add_undo,nullptr,undo_snapshot);
        add_hook(method(api.interface_edit,"ControlUndo",0),undo_prefix,history_done,true);
        add_hook(method(api.interface_edit,"ControlRedo",0),redo_prefix,history_done,true);
        add_hook(method(api.interface_edit,"Backup",0),nullptr,backup_postfix);
        add_hook(method(api.interface_edit,"CancelChanges",0),nullptr,cancel_postfix);
        add_hook(method(api.interface_edit,"ClearControlUndoRedo",0),nullptr,history_postfix);
        add_hook(method(api.interface_edit,"ClearControlRedo",0),nullptr,redo_clear);
        add_hook(method(api.interface_edit,"RestoreBackup",0),nullptr,cancel_postfix);
        add_hook(method(api.interface_edit,"CopyProfile",0),nullptr,copy_postfix);
        add_hook(method(api.interface_edit,"PasteProfile",0),nullptr,paste_postfix);
        add_hook(method(api.interface_edit,"RestoreDefaults",0),nullptr,default_postfix);
        add_hook(method(api.styles,"SaveConfiguration",1),save_prefix,save_postfix);
        add_hook(method(api.styles,"SetAllConfigurationsToDefault",0),nullptr,reset_postfix);
        ready=true;reason.clear();LOG_INFO("[controls] native layout binding hooks installed: {}",hooks.size());
    }catch(const std::exception& e){reason=e.what();for(auto h:hooks)patchlib_uninstall_hook(h);hooks.clear();LOG_WARN("[controls] native layout binding unavailable: {}",reason);}
#endif
}
void shutdown(){
#if defined(__ANDROID__) && defined(__aarch64__)
    for(auto it=hooks.rbegin();it!=hooks.rend();++it)patchlib_uninstall_hook(*it);
    hooks.clear();restore_render();clear_input();rows.clear();module_ids.clear();popup_module=0;drawing.clear();selected.reset();popup_owner=nullptr;runtime_depth=0;
#endif
    ready=false;state.actions.clear();state.clear_history();state.backup.clear();state.clipboard.clear();state.has_backup=false;state.has_clipboard=false;
}
void unregister_owner(lua_mod_handle_t* h){
    if(!h)return;
#if defined(__ANDROID__) && defined(__aarch64__)
    for(auto& [key,a]:state.actions)if(a.owner==h->mod_id)invalidate(key,true);
#endif
    state.remove_owner(h->mod_id);
    refresh_modules();
}
void register_api(lua_State* L,lua_mod_handle_t* h){
    lua_newtable(L);
    const luaL_Reg funcs[]={{"available",available},{"register",add},{"unregister",remove},{"down",down},{"pressed",pressed},{"released",released},{"set_enabled",enable},{"set_visible",show},{nullptr,nullptr}};
    for(auto f=funcs;f->name;++f){lua_pushlightuserdata(L,h);lua_pushcclosure(L,f->func,1);lua_setfield(L,-2,f->name);}
    lua_setfield(L,-2,"controls");
}
}
