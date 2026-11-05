/*******************************************************************************
 * LuaLoader - lua_api internal
 * Copyright (C) 2026 lzup333
 *
 * lua_api 各模块之间共享的声明。实现分布在 src/api 目录下的各编译单元。
 *******************************************************************************/

#pragma once

#include "lua_api.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <utility>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "logger.hpp"
#ifdef LUALOADER_GUI
#include "gui.hpp"
#endif

#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/patchlib/struct/dictionary.h"
#include "tefkernel/patchlib/struct/list.h"
#include "tefkernel/patchlib/struct/string.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/tefstd/vector.h"

#include "tefkernel/terraria/asset.h"
#include "tefkernel/terraria/content/item_manager.h"
#include "tefkernel/terraria/content/projectile_manager.h"
#include "tefkernel/terraria/content/npc_manager.h"
#include "tefkernel/terraria/content/buff_manager.h"
#include "tefkernel/terraria/netmanager.h"
#include "tefkernel/terraria/language_manager.h"
#include "tefkernel/terraria/recipe_manager.h"
#include "tefkernel/terraria/texture2d.h"

#include "image_decode.hpp"

namespace lualoader::lua_api {

    static constexpr const char *HANDLE_KEY = "lualoader.mod.handle";
    static constexpr const char *HOOK_METATABLE = "LuaLoader.Hook";
    static constexpr const char *OWNED_HANDLE_MT = "LuaLoader.OwnedHandle";
    static constexpr int MAX_HOOK_SLOTS = 1024;
    static constexpr int MAX_CALL_ARGS = 16;
    static constexpr const char *LUALOADER_PKG_ID = "lzup333.lualoader";

    struct OwnedHandle {
        patch_handle_t handle;
        void *root; ///< Android：托管对象的 GC 强引用
    };

    /// 判断某个 entry 记录的回调上下文是否仍然可用：
    /// entry 及其 owner handle 都必须存活，且 entry 缓存的 Lua 状态机必须与
    /// owner 当前的状态机一致（handle 被 close 后 handle->L 置空，此处即失效）。
    /// 用于在 SetDefaults / AI / hook 等异步回调入口兜底，防止对已 lua_close 的
    /// 状态机执行 lua_rawgeti / lua_next 等操作（会触发原生 SIGSEGV）。
    inline bool call_ctx_alive(const lua_State *L, const lua_mod_handle_t *owner) {
        return L != nullptr && owner != nullptr && owner->L == L;
    }

    // ---- 基础工具 ----
    lua_mod_handle_t *get_handle(lua_State *L);
    patch_handle_t copy_handle(patch_handle_t h);
    void release_owned_handle(OwnedHandle *ud);
    void push_owned_handle(lua_State *L, patch_handle_t h);
    /// 内核为“返回值”新分配的引用句柄：桌面端交给 Lua GC 释放，Android 保持借用
    void push_returned_object(lua_State *L, patch_handle_t h);
    void free_returned_object(patch_handle_t h);
    int l_owned_handle_gc(lua_State *L);
    void push_handle(lua_State *L, patch_handle_t h);
    patch_handle_t to_handle(lua_State *L, int idx);
    void push_patch_value(lua_State *L, patch_type_t type, const void *value);
    bool read_patch_value(lua_State *L, int idx, patch_type_t type, void *out);
    bool try_read_patch_value(lua_State *L, int idx, patch_type_t type, void *out);
    int pcall_log(lua_State *L, int nargs, int nresults, const char *what);

    // ---- 日志 / 文件（register_api 直接挂到 mod 表）----
    int l_mod_log(lua_State *L);
    int l_mod_trace(lua_State *L);
    int l_mod_debug(lua_State *L);
    int l_mod_info(lua_State *L);
    int l_mod_warn(lua_State *L);
    int l_mod_error(lua_State *L);
    int l_mod_fatal(lua_State *L);
    int l_mod_read_file(lua_State *L);
    int l_mod_write_file(lua_State *L);
    int l_mod_file_exists(lua_State *L);

    // ---- 物品 / 弹幕共用 ----
    struct LuaItemFieldValue {
        patch_handle_t field;
        patch_type_t type;
        uint64_t value;
        int ref; ///< 引用类型（object/pointer）字段的 Lua registry 引用，保活来源；数值字段为 LUA_NOREF
    };
    int ref_default_value(lua_State *L, int idx, patch_type_t type);
    void unref_default_values(lua_State *L, std::vector<LuaItemFieldValue> &defaults);
    void item_field_write(patch_handle_t field, patch_handle_t inst,
                          patch_type_t type, const void *value);
    void premultiply_alpha(unsigned char *buf, size_t sz);
    std::string resolve_item_image(const std::string &private_dir,
                                   const std::string &mod_dir,
                                   const std::string &rel);

    // ---- patch 内部共用 ----
    bool type_from_name(const char *name, patch_type_t *out);
    void push_managed_string(lua_State *L, patch_handle_t str);
    bool instance_missing(lua_State *L, int idx, patch_handle_t field);

    // ---- 结构体原语 ----
    size_t patch_scalar_size(patch_type_t type);

    // ---- 钩子 ----
    extern std::atomic<int> g_active_hook_slot;
    extern std::atomic<int> g_active_hook_phase;
    int l_hook_remove(lua_State *L);
    void release_slot(int slot);

    // ---- 各模块把自己那部分挂到栈顶的表上 ----
    void add_patch_core(lua_State *L);
    void add_patch_array(lua_State *L);
    void add_patch_inspect(lua_State *L);
    void add_patch_container(lua_State *L);
    void add_patch_struct(lua_State *L);
    void add_patch_hook(lua_State *L);
    void add_net(lua_State *L);
    void add_recipe(lua_State *L);
    void add_item(lua_State *L);
    void add_projectile(lua_State *L);
    void add_buff(lua_State *L);
    void add_npc(lua_State *L);

    // ---- 配方清理 ----
    void unregister_recipes(lua_mod_handle_t *handle);
    void unregister_buffs(lua_mod_handle_t *handle);
    void unregister_npcs(lua_mod_handle_t *handle);
    void shutdown_item_hooks();
    void shutdown_buff_hooks();
    void shutdown_recipe_hooks();

} // namespace lualoader::lua_api
