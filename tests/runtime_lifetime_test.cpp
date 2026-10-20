// Host regression tests: real Lua/API/engine/core; only game-owned callbacks are stubbed.
#include "../src/lua_api.cpp"
#include "../src/lua_engine.cpp"
#include "../src/core.cpp"
#include <cassert>
#include <iostream>

using namespace lualoader;
static int uninstalls = 0;
static int installs = 0;
static lua_mod_handle_t *registration_owner = nullptr;
static bool items_removed_while_alive = false;
static bool projectiles_removed_while_alive = false;
static patch_hook_id_t install_hook(patch_handle_t, prefix_callback_t, postfix_callback_t) {
    if (registration_owner) {
        auto *item = new lua_api::LuaItemEntry{};
        item->owner = registration_owner;
        item->L = registration_owner->L;
        item->registered = true;
        item->item.runtime_id = 10001;
        item->defaults_ref = item->can_use_ref = LUA_NOREF;
        lua_api::g_lua_items.push_back(item);
        auto *projectile = new lua_api::LuaProjectileEntry{};
        projectile->owner = registration_owner;
        projectile->L = registration_owner->L;
        projectile->registered = true;
        projectile->projectile.runtime_id = 10001;
        projectile->defaults_ref = projectile->ai_ref = LUA_NOREF;
        lua_api::g_lua_projectiles.push_back(projectile);
    }
    return static_cast<patch_hook_id_t>(++installs);
}
static bool uninstall_hook(patch_hook_id_t) { ++uninstalls; return true; }
static bool remove_item(int) {
    items_removed_while_alive = registration_owner && registration_owner->L;
    return true;
}
static bool remove_projectile(int) {
    projectiles_removed_while_alive = registration_owner && registration_owner->L;
    return true;
}
static void run(lua_State *L, const char *script) {
    const int result = luaL_dostring(L, script);
    if (result) std::cerr << lua_tostring(L, -1) << '\n';
    assert(result == LUA_OK);
}
static size_t empty_vector(const tefstd_vector_t *) { return 0; }
static std::string contents(const std::filesystem::path &path) {
    std::ifstream in(path); return std::string(std::istreambuf_iterator<char>(in), {});
}
static void check_stack(lua_State *L) {
    assert(lua_gettop(L) == 1 && lua_tointeger(L, 1) == 12345 && "callback damaged caller stack");
}
int main(int argc, char **argv) {
    assert(argc == 3);
    const std::string mode = argv[1];
    const std::filesystem::path root = argv[2];
    std::filesystem::create_directories(root);
    patchlib_install_prepost_hook = install_hook;
    patchlib_uninstall_hook = uninstall_hook;
    terraria_item_manager_unregister_item_by_id = remove_item;
    terraria_projectile_manager_unregister_projectile_by_id = remove_projectile;
    if (mode.rfind("callback_", 0) == 0) {
        auto *L = luaL_newstate(); luaL_openlibs(L);
        run(L, "function fail_callback() error('expected callback error') end");
        lua_getglobal(L, "fail_callback");
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_api::LuaItemEntry item{}; item.L = L; item.defaults_ref = item.can_use_ref = ref;
        lua_api::LuaProjectileEntry projectile{}; projectile.L = L;
        projectile.defaults_ref = projectile.ai_ref = ref;
        for (int i = 0; i < 3; ++i) {
            lua_pushinteger(L, 12345);
            if (mode == "callback_item_defaults") lua_api::lua_item_set_defaults(&item.item, reinterpret_cast<void *>(1));
            else if (mode == "callback_can_use") assert(lua_api::lua_item_can_use(&item.item, nullptr, nullptr, false));
            else if (mode == "callback_projectile_defaults") lua_api::lua_projectile_set_defaults(&projectile.projectile, reinterpret_cast<void *>(1));
            else lua_api::lua_projectile_ai(&projectile.projectile, reinterpret_cast<void *>(1));
            check_stack(L); lua_pop(L, 1);
        }
        lua_close(L);
    } else if (mode.rfind("result_", 0) == 0) {
        lua_mod_handle_t h; h.L = luaL_newstate(); luaL_openlibs(h.L);
        lua_api::register_api(h.L, &h);
        lua_pushlightuserdata(h.L, reinterpret_cast<void *>(1)); lua_setglobal(h.L, "method");
        const bool prefix = mode.find("prefix") != std::string::npos;
        const bool bad = mode.find("bad") != std::string::npos;
        const std::string value = bad ? "{}" : "77";
        run(h.L, (prefix ? "hook = mod.patch.install_hook(method, {prefix=function() return true, " + value + " end})"
                        : "hook = mod.patch.install_hook(method, {override_result=true, postfix=function() return " + value + " end})").c_str());
        patch_method_signature_t sig{}; sig.return_type = PATCH_INT32;
        tefstd_vector_size = empty_vector;
        int32_t result = 11;
        lua_pushinteger(h.L, 12345);
        if (prefix) {
            const bool skip = lua_api::hook_prefix_dispatch(h.hook_slots.at(0), nullptr, nullptr, &sig, &result);
            assert(skip == !bad && "invalid override must allow original method");
        } else lua_api::hook_postfix_dispatch(h.hook_slots.at(0), nullptr, nullptr, &result, &sig);
        assert(result == (bad ? 11 : 77)); check_stack(h.L);
        lua_pop(h.L, 1); lua_engine::close(&h);
    } else if (mode == "native_priority" || mode == "native_stale") {
        const auto high = root / "arm64"; const auto low = root / "generic"; const auto dst = root / "cache";
        std::filesystem::create_directories(high); std::filesystem::create_directories(low);
        std::ofstream(high / "module.so") << "arm64";
        std::ofstream(low / "module.so") << "generic";
        std::ofstream(low / "old.so") << "old";
        const std::vector<std::string> dirs{high.string(), low.string()};
        assert(lua_engine::stage_native_libs(dirs, dst.string(), "test.mod"));
        if (mode == "native_priority") assert(contents(dst / "module.so") == "arm64" && "lower priority library overwrote preferred binary");
        else {
            std::filesystem::remove(low / "old.so");
            assert(lua_engine::stage_native_libs(dirs, dst.string(), "test.mod"));
            assert(!std::filesystem::exists(dst / "old.so") && "removed library remains loadable");
            std::filesystem::remove(high / "module.so"); std::filesystem::remove(low / "module.so");
            assert(lua_engine::stage_native_libs(dirs, dst.string(), "test.mod"));
            assert(!std::filesystem::exists(dst / "module.so"));
        }
    } else if (mode == "hook_reuse") {
        lua_mod_handle_t h;
        h.L = luaL_newstate(); luaL_openlibs(h.L);
        lua_api::register_api(h.L, &h);
        lua_pushlightuserdata(h.L, reinterpret_cast<void *>(1)); lua_setglobal(h.L, "method");
        run(h.L, "old = mod.patch.install_hook(method, {prefix=function() end})");
        lua_engine::uninstall_hooks(&h);
        assert(uninstalls == 1);
        run(h.L, "current = mod.patch.install_hook(method, {prefix=function() end}); old:remove()");
        assert(uninstalls == 1 && "old handle removed a newly installed hook");
        run(h.L, "current:remove(); current:remove()");
        assert(uninstalls == 2);
        lua_engine::close(&h);
    } else if (mode == "load_failure") {
        std::ofstream(root / "main.lua") << "mod.patch.install_hook(debug.getregistry()['lualoader.mod.handle'], {prefix=function() end}); error('entry failure')";
        lua_mod_handle_t h;
        registration_owner = &h;
        std::string err;
        assert(!lua_engine::load(&h, (root / "mod.json").string(), root.string(), err));
        assert(err.find("entry failure") != std::string::npos);
        assert(items_removed_while_alive && projectiles_removed_while_alive && "callbacks outlived Lua state");
        assert(lua_api::g_lua_items.empty() && lua_api::g_lua_projectiles.empty());
        assert(!h.L && h.native_libs.empty() && h.hook_slots.empty());
        lua_engine::close(&h);
    } else {
        std::ofstream(root / "main.lua") << "mod.init = function() end";
        std::ofstream(root / "mod.json") << "{}";
        const auto cfg = (root / "mod.json").string();
        const auto priv = root.string();
        mod_manifest_t m{}; m.mod_id = "test.mod"; m.path = cfg.c_str();
        m.private_dir = mode == "null_private" ? nullptr : priv.c_str();
        assert(core::load_mod(&m) == ML_SUCCESS);
        auto *original = core::lua_mod_handles.at(m.mod_id);
        if (mode == "duplicate") {
            assert(core::load_mod(&m) == ML_SUCCESS);
            assert(core::lua_mod_handles.at(m.mod_id) == original);
            assert(installs == 0);
            // A second execution would change this persistent marker.
            lua_pushinteger(original->L, 42); lua_setglobal(original->L, "marker");
            std::ofstream(root / "main.lua") << "error('duplicate entry executed')";
            assert(core::load_mod(&m) == ML_SUCCESS);
        }
        assert(core::unload_mod(&m) == ML_SUCCESS);
        assert(core::lua_mod_handles.empty() && core::lua_mod_multiplayer_infos.empty());
    }
    std::cout << "PASS " << mode << '\n';
}
