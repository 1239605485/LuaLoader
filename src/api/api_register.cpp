#if defined(__ANDROID__)
#include "controls.hpp"
#endif
#include "api_internal.hpp"

namespace lualoader::lua_api {

    void register_api(lua_State *L, lua_mod_handle_t *handle) {
        lua_pushlightuserdata(L, handle);
        lua_setfield(L, LUA_REGISTRYINDEX, HANDLE_KEY);

        // 托管句柄元表：__gc 由 Lua GC 自动释放内核句柄（Android 上释放为空操作）
        luaL_newmetatable(L, OWNED_HANDLE_MT);
        lua_pushcfunction(L, l_owned_handle_gc);
        lua_setfield(L, -2, "__gc");
        lua_pop(L, 1);

        // 钩子句柄元表：不再注册 __gc 自动卸载，钩子只在显式 :remove()
        // 或 Mod 卸载时移除（否则丢弃返回值会被 Lua GC 误卸载）
        luaL_newmetatable(L, HOOK_METATABLE);
        lua_pushcfunction(L, l_hook_remove);
        lua_setfield(L, -2, "remove");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pop(L, 1);

        // 全局 mod 表
        lua_newtable(L);

        lua_pushstring(L, handle->mod_id.c_str());
        lua_setfield(L, -2, "id");
        lua_pushstring(L, handle->mod_dir.c_str());
        lua_setfield(L, -2, "mod_dir");
        lua_pushstring(L, handle->private_dir.c_str());
        lua_setfield(L, -2, "private_dir");
        lua_pushstring(L, handle->platform.c_str());
        lua_setfield(L, -2, "platform");
        lua_pushstring(L, handle->arch.c_str());
        lua_setfield(L, -2, "arch");
        lua_pushstring(L, handle->info.version.c_str());
        lua_setfield(L, -2, "version");

        lua_pushcfunction(L, l_mod_log);
        lua_setfield(L, -2, "log");
        lua_pushcfunction(L, l_mod_trace);
        lua_setfield(L, -2, "trace");
        lua_pushcfunction(L, l_mod_debug);
        lua_setfield(L, -2, "debug");
        lua_pushcfunction(L, l_mod_info);
        lua_setfield(L, -2, "info");
        lua_pushcfunction(L, l_mod_warn);
        lua_setfield(L, -2, "warn");
        lua_pushcfunction(L, l_mod_error);
        lua_setfield(L, -2, "error");
        lua_pushcfunction(L, l_mod_fatal);
        lua_setfield(L, -2, "fatal");

        lua_pushcfunction(L, l_mod_read_file);
        lua_setfield(L, -2, "read_file");
        lua_pushcfunction(L, l_mod_write_file);
        lua_setfield(L, -2, "write_file");
        lua_pushcfunction(L, l_mod_file_exists);
        lua_setfield(L, -2, "file_exists");

        lua_newtable(L);
        add_patch_core(L);
        add_patch_array(L);
        add_patch_inspect(L);
        add_patch_container(L);
        add_patch_struct(L);
        add_patch_hook(L);
        lua_setfield(L, -2, "patch");

        lua_newtable(L);
        add_item(L);
        lua_setfield(L, -2, "item");

        lua_newtable(L);
        add_projectile(L);
        lua_setfield(L, -2, "projectile");

        lua_newtable(L);
        add_buff(L);
        lua_setfield(L, -2, "buff");

        lua_newtable(L);
        add_npc(L);
        lua_setfield(L, -2, "npc");

        lua_newtable(L);
        add_net(L);
        lua_setfield(L, -2, "net");

        lua_newtable(L);
        add_recipe(L);
        lua_setfield(L, -2, "recipe");

#ifdef LUALOADER_GUI
        lualoader::gui::register_api(L, handle);
#endif

#if defined(__ANDROID__)
        /* TODO(button): 内核 button_manager 被上游 revert，暂时禁用
        controls::register_api(L, handle);
        */
#endif
        lua_pushvalue(L, -1);
        lua_setglobal(L, "mod");
        lua_pop(L, 1);
    }

    void uninstall_all_hooks(lua_mod_handle_t *handle) {
        if (!handle) return;
        const std::vector<int> slots = handle->hook_slots;
        for (const int slot: slots) {
            release_slot(slot);
        }
    }

    void shutdown_global_hooks() {
        shutdown_item_hooks();
        shutdown_buff_hooks();
        shutdown_recipe_hooks();
    }

} // namespace lualoader::lua_api
