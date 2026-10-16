#include "lua_api.hpp"
#include "json.hpp"
#include "tefkernel/terraria/item_manager.h"
#include "tefkernel/terraria/language_manager.h"
#include "tefkernel/terraria/recipe_manager.h"
#include <cassert>
#include <cstdio>
#include <vector>
extern "C" {
#include "lualib.h"
}

static std::vector<terraria_item_handle_t *> registered;
static terraria_languager_manager_get_localized_text_t language_callback;
static terraria_recipe_manager_addrecipes_t recipe_callback;
static int language_registrations, recipe_registrations, results, recipe_id, recipe_stack, recipe_station;
static std::vector<int> materials;
static bool locked;
static bool register_item(terraria_item_handle_t *item) {
    if (locked) return false;
    registered.push_back(item);
    return true;
}
static bool unregister_item(terraria_item_handle_t *item) {
    for (auto it = registered.begin(); it != registered.end(); ++it)
        if (*it == item) { registered.erase(it); return true; }
    return false;
}
static void run(lua_State *L, const char *code) {
    if (luaL_dostring(L, code) != LUA_OK) {
        std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
        std::abort();
    }
    assert(lua_gettop(L) == 0);
}
int main() {
    terraria_item_manager_get_api_version = [] { return 1; };
    terraria_item_manager_register_item = register_item;
    terraria_item_manager_unregister_item = unregister_item;
    terraria_language_manager_register_callback = [](auto cb) { language_callback = cb; ++language_registrations; };
    terraria_recipe_manager_register_callback = [](auto cb) { recipe_callback = cb; ++recipe_registrations; };
    terraria_recipe_set_result = [](int id, int stack) { recipe_id = id; recipe_stack = stack; };
    terraria_recipe_set_material = [](int *mats, int count) { materials.assign(mats, mats + count * 2); };
    terraria_recipe_set_station = [](int id) { recipe_station = id; };
    terraria_recipe_add = [] { ++results; };
    patchlib_handle_copy = [](patch_handle_t h) { return h; };
    patchlib_free = [](patch_handle_t) {};
    patchlib_is_valid = [](patch_handle_t h) { return h != nullptr; };
    lualoader::lua_mod_handle_t owner;
    owner.mod_id = "runtime-id";
    owner.info.pkg_id = "test.items";
    owner.L = luaL_newstate();
    luaL_openlibs(owner.L);
    lualoader::lua_api::register_api(owner.L, &owner);
    run(owner.L, R"(
        assert(mod.items.available())
        local ok = pcall(mod.items.register, {internal_name='bad.name'})
        assert(not ok)
        assert(not pcall(mod.items.register, {internal_name='Bad',set_defaults=3}))
        assert(not pcall(mod.items.register, {internal_name='Bad',recipe={materials={{1,0}}}}))
        assert(mod.items.register {internal_name='Ore',display_name='测试矿石',tooltip='自定义材料'} == 'Ore')
        assert(mod.items.get_id('Ore') == nil)
        mod.items.register {
            internal_name='Sword',display_name='测试剑',
            early_init=function(id) assert(id==7001); early=true end,
            set_defaults=function(item,id) assert(item and id==7001); defaults=true end,
            can_use=function(player,item,ignore,id) assert(player and item and id==7001); return ignore end,
            recipe={materials={{'Ore',3},{9,2}},stack=2,station=18}
        }
        assert(not pcall(mod.items.register, {internal_name='Ore'}))
    )");
    assert(registered.size() == 2);
    assert(language_registrations == 1 && recipe_registrations == 1);
    auto text = nlohmann::json::parse(language_callback(LANGUAGE_CHINESE_SIMPLIFIED));
    assert(text["item_name_lzup333.lualoader_test.items"]["Ore"] == "测试矿石");
    registered[0]->runtime_id = 7000;
    registered[1]->runtime_id = 7001;
    auto *sword = registered[1];
    sword->item_ops.early_init(sword);
    sword->item_ops.set_defaults(sword, reinterpret_cast<void *>(1));
    assert(!sword->item_ops.can_use(sword, reinterpret_cast<void *>(2), reinterpret_cast<void *>(1), false));
    assert(sword->item_ops.can_use(sword, reinterpret_cast<void *>(2), reinterpret_cast<void *>(1), true));
    assert(lua_gettop(owner.L) == 0);
    run(owner.L, "assert(early and defaults and mod.items.get_id('Sword') == 7001)");
    recipe_callback();
    assert(results == 1 && recipe_id == 7001 && recipe_stack == 2 && recipe_station == 18);
    assert((materials == std::vector<int>{7000,3,9,2}));
    locked = true;
    run(owner.L, "assert(not pcall(mod.items.register,{internal_name='Late'}))");
    assert(registered.size() == 2);
    locked = false;
    run(owner.L, R"(
        mod.items.register {internal_name='Broken',can_use=function() error('expected test error') end}
        mod.items.register {internal_name='Temporary'}
        assert(mod.items.unregister('Temporary'))
        assert(not mod.items.unregister('Temporary'))
    )");
    auto *broken = registered.back();
    assert(!broken->item_ops.can_use(broken, reinterpret_cast<void *>(2), reinterpret_cast<void *>(1), false));
    assert(broken->item_ops.can_use(broken, reinterpret_cast<void *>(2), reinterpret_cast<void *>(1), false));
    assert(lua_gettop(owner.L) == 0);
    lualoader::lua_api::unregister_all_items(&owner);
    assert(registered.empty());
    assert(nlohmann::json::parse(language_callback(LANGUAGE_ENGLISH)).empty());
    results = 0; recipe_callback(); assert(results == 0);
    run(owner.L, "mod.items.register {internal_name='Ore'}");
    assert(language_registrations == 1 && recipe_registrations == 1);
    lualoader::lua_api::unregister_all_items(&owner);
    terraria_item_manager_get_api_version = nullptr;
    run(owner.L, "assert(not mod.items.available()); assert(not pcall(mod.items.register,{internal_name='Old'}))");
    lua_close(owner.L);
    owner.L = nullptr;
    std::puts("item API tests passed");
}
