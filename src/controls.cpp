#if defined(__ANDROID__)
#include "controls.hpp"
#include "logger.hpp"
#include "tefkernel/terraria/content/button_manager.h"
#include <cstring>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace lualoader::controls {
namespace {
lua_mod_handle_t* owner(lua_State* L){return static_cast<lua_mod_handle_t*>(lua_touserdata(L,lua_upvalueindex(1)));}
bool linked(){return terraria_button_manager_available&&terraria_button_manager_error&&terraria_button_manager_configure_storage&&terraria_button_manager_install&&terraria_button_manager_shutdown&&terraria_button_manager_register&&terraria_button_manager_unregister&&terraria_button_manager_unregister_owner&&terraria_button_manager_down&&terraria_button_manager_pressed&&terraria_button_manager_released&&terraria_button_manager_set_enabled&&terraria_button_manager_set_visible;}
const char* error(){return linked()?terraria_button_manager_error():"kernel button_manager unavailable";}
int available(lua_State* L){lua_pushboolean(L,linked()&&terraria_button_manager_available());lua_pushstring(L,error());return 2;}
int add(lua_State* L){
 if(!linked()||!terraria_button_manager_available())return luaL_error(L,"native touch bindings unavailable: %s",error());
    luaL_checktype(L,1,LUA_TTABLE);
    lua_getfield(L,1,"id");std::string id=luaL_checkstring(L,-1);lua_pop(L,1);
    if(id.empty()||id.size()>64||id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")!=std::string::npos)return luaL_error(L,"control id must be 1-64 ASCII letters, digits, _, - or .");
    lua_getfield(L,1,"name");std::string name=luaL_checkstring(L,-1);lua_pop(L,1);
    if(name.empty()||name.size()>128)return luaL_error(L,"control name must be 1-128 UTF-8 bytes");
    lua_getfield(L,1,"icon");std::string icon_path;
    if(!lua_isnil(L,-1)){
        luaL_checktype(L,-1,LUA_TSTRING);size_t length=0;
        const char* path=lua_tolstring(L,-1,&length);
        if(length>512||std::memchr(path,0,length))return luaL_error(L,"control icon path must be at most 512 bytes without NUL");
        icon_path.assign(path,length);
    }
    lua_pop(L,1);
    lua_getfield(L,1,"show_label");bool show_label=true;
    if(!lua_isnil(L,-1)){luaL_checktype(L,-1,LUA_TBOOLEAN);show_label=lua_toboolean(L,-1);}
    lua_pop(L,1);

 auto h=owner(L);
 terraria_button_desc_t desc{h->mod_id.c_str(),id.c_str(),name.c_str(),h->private_dir.c_str(),icon_path.c_str(),show_label};
 if(!terraria_button_manager_register(&desc))return luaL_error(L,"control registration failed: %s",error());
 lua_pushlstring(L,id.data(),id.size());return 1;
}
int remove(lua_State* L){auto h=owner(L);const char* id=luaL_checkstring(L,1);lua_pushboolean(L,linked()&&terraria_button_manager_unregister(h->mod_id.c_str(),id));return 1;}
int down(lua_State* L){auto h=owner(L);const char* id=luaL_checkstring(L,1);lua_pushboolean(L,linked()&&terraria_button_manager_down(h->mod_id.c_str(),id));return 1;}
int pressed(lua_State* L){auto h=owner(L);const char* id=luaL_checkstring(L,1);lua_pushboolean(L,linked()&&terraria_button_manager_pressed(h->mod_id.c_str(),id));return 1;}
int released(lua_State* L){auto h=owner(L);const char* id=luaL_checkstring(L,1);lua_pushboolean(L,linked()&&terraria_button_manager_released(h->mod_id.c_str(),id));return 1;}
int flag(lua_State* L,bool visible){auto h=owner(L);const char* id=luaL_checkstring(L,1);bool value=lua_toboolean(L,2);
 if(!linked()||!(visible?terraria_button_manager_set_visible(h->mod_id.c_str(),id,value):terraria_button_manager_set_enabled(h->mod_id.c_str(),id,value)))return luaL_error(L,"control not registered");return 0;}
int enable(lua_State* L){return flag(L,false);}int show(lua_State* L){return flag(L,true);}
}
void configure_storage(const std::string& directory){if(linked())terraria_button_manager_configure_storage(directory.c_str());}
void install(){
 if(!linked()){LOG_ERROR("[button-interface] kernel button_manager exports missing; install the modified PE kernel");return;}
 terraria_button_manager_install();
 if(!terraria_button_manager_available())LOG_ERROR("[button-interface] kernel initialization failed: {}",error());
 else LOG_INFO("[button-interface] ready: native PE buttons and PNG icons");
}
void shutdown(){/* Kernel owns hooks; unloading Lua only unregisters its actions. */}
void append_image_owners(std::vector<std::string>&){}
void unregister_owner(lua_mod_handle_t* h){if(h&&linked())terraria_button_manager_unregister_owner(h->mod_id.c_str());}
void register_api(lua_State* L,lua_mod_handle_t* h){
    lua_newtable(L);
    const luaL_Reg funcs[]={{"available",available},{"register",add},{"unregister",remove},{"down",down},{"pressed",pressed},{"released",released},{"set_enabled",enable},{"set_visible",show},{nullptr,nullptr}};
    for(auto f=funcs;f->name;++f){lua_pushlightuserdata(L,h);lua_pushcclosure(L,f->func,1);lua_setfield(L,-2,f->name);}
    lua_setfield(L,-2,"controls");
}
}

#endif
