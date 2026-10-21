#pragma once
#include <string>
#include "lua_engine.hpp"
namespace lualoader::controls {
void configure_storage(const std::string& directory);
void install();
void shutdown();
void register_api(lua_State* L,lua_mod_handle_t* owner);
void unregister_owner(lua_mod_handle_t* owner);
}
