#include "system_keyboard.hpp"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/string.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <cstdio>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
static int status, opened, closed, strings;
static std::string typed;
static void *h(int n) { return reinterpret_cast<void *>(static_cast<uintptr_t>(n)); }
int main() {
    using namespace lualoader::text_input;
    assert(!available());
    patchlib_type_get_type=[](const char *ns,const char *name)->void* {
        assert(std::string(ns)=="UnityEngine" && std::string(name)=="TouchScreenKeyboard"); return h(1);
    };
    patchlib_type_get_method_by_param_count=[](void*,const char *name,int count)->void* {
        if (!strcmp(name,"Open")) { assert(count==8); return h(2); }
        if (!strcmp(name,"get_text")) return h(3);
        if (!strcmp(name,"get_status")) return h(4);
        if (!strcmp(name,"set_active")) return h(5);
        return nullptr;
    };
    patchlib_string_create=[](const char *text)->void* { ++strings; return new std::string(text); };
    patchlib_string_cstr=[](void *text)->char* { return strdup(static_cast<std::string *>(text)->c_str()); };
    patchlib_free=[](void *value) {
        if (reinterpret_cast<uintptr_t>(value)>4096) { delete static_cast<std::string *>(value); --strings; }
    };
    patchlib_method_invoke_args=[](void *method,void *instance,void *result,void **args)->bool {
        if (method==h(2)) {
            assert(!instance && *static_cast<int *>(args[1])==0);
            typed=*static_cast<std::string *>(*static_cast<void **>(args[0]));
            status=0;++opened;*static_cast<void **>(result)=h(6);return true;
        }
        assert(instance==h(6));
        if (method==h(3)) { ++strings;*static_cast<void **>(result)=new std::string(typed);return true; }
        if (method==h(4)) { *static_cast<int *>(result)=status;return true; }
        if (method==h(5)) { assert(!*static_cast<bool *>(args[0]));++closed;return true; }
        return false;
    };
    assert(available());
    Options options;options.text="原始中文";options.placeholder="输入名称";
    std::string error;
    assert(open("a#search",options,error) && error.empty());
    assert(read("b").status=="idle");
    assert(!open("b",options,error) && error=="native text input is busy");
    typed="天顶剑😀";
    assert(read("a#search").text=="天顶剑😀");
    status=1;
    assert(read("a#search").status=="done" && read("a#search").text=="天顶剑😀");
    assert(open("a#search",options,error));typed="取消后的值";status=2;
    assert(read("a#search").status=="canceled" && read("a#search").text=="原始中文");
    assert(open("a#search",options,error));typed="保留";close("a#search",true);
    assert(read("a#search").status=="done" && read("a#search").text=="保留");
    assert(open("a#search",options,error));typed="焦点切换";status=3;
    assert(read("a#search").status=="lost_focus" && read("a#search").text=="焦点切换");
    assert(open("a#search",options,error));release_mod("b");assert(read("a#search").status=="visible");
    release_mod("a");assert(read("a#search").status=="idle");
    assert(opened==5 && closed==2 && strings==0);
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    register_api(L, "lua-test");
    lua_setglobal(L, "mod");
    assert(luaL_dostring(L, "assert(mod.text_input.api_version==1 and mod.text_input.available()); assert(mod.text_input.open{text='中文',placeholder='搜索',limit=20})") == LUA_OK);
    typed = "天顶剑"; status = 1;
    assert(luaL_dostring(L, "local text,state,err=mod.text_input.read(); assert(text=='天顶剑' and state=='done' and err==nil); mod.text_input.close()") == LUA_OK);
    release_mod("lua-test");
    shutdown();
    lua_close(L);
    assert(strings==0);
    puts("System keyboard Chinese/emoji, Lua API, cancel, completion, focus, ownership and cleanup tests passed");
}
