#ifndef LUALOADER_SUPPORT_API_H
#define LUALOADER_SUPPORT_API_H
#include "lua.h"
#ifdef __cplusplus
extern "C" {
#endif
/* mod table must be at the top of the Lua stack. PE only. */
void lualoader_support_register(lua_State *L);
void lualoader_support_close(lua_State *L);
#ifdef __cplusplus
}
#endif
#endif
