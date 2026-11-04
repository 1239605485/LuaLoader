#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ========================================================================
    // mod.npc：Lua 侧注册自定义 NPC
    //
    //   mod.npc.register{
    //     name     = "my_slime",
    //     texture  = "slime.png",
    //     frames   = 1,
    //     defaults = { width=32, height=32, lifeMax=100, damage=10, ... },
    //     ai       = function(npc) end,
    //   } -> runtime_id | nil
    //
    //   mod.npc.spawn("my_slime", x, y)
    // ========================================================================

    struct LuaNpcLoot {
        int item_id = 0;
        int count = 1;
    };

    struct LuaNpcEntry {
        terraria_npc_handle_t npc; // 必须是首成员
        lua_mod_handle_t *owner;
        lua_State *L;
        std::string name;
        std::vector<LuaItemFieldValue> defaults;
        int defaults_ref;
        int ai_ref;
        int on_death_ref;
        std::vector<LuaNpcLoot> loot;
        patch_handle_t f_pos;
        patch_handle_t f_width;
        patch_handle_t f_height;
        std::string texture_path;
        std::vector<unsigned char> tex_pixels;
        int tex_w, tex_h;
        bool tex_tried;
        bool registered;
        int runtime_id;
    };

    static std::vector<LuaNpcEntry *> g_lua_npcs;

    static LuaNpcEntry *npc_from_handle(terraria_npc_handle_t *h) {
        return reinterpret_cast<LuaNpcEntry *>(h);
    }

    static void lua_npc_set_defaults(terraria_npc_handle_t *current, patch_handle_t inst) {
        LuaNpcEntry *e = npc_from_handle(current);
        if (!e || !inst) return;
        for (const auto &d: e->defaults)
            if (patchlib_is_valid(d.field)) item_field_write(d.field, inst, d.type, &d.value);
        if (e->defaults_ref != LUA_NOREF && e->L) {
            lua_State *L = e->L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, e->defaults_ref);
            push_handle(L, inst);
            pcall_log(L, 1, 0, "mod.npc.defaults");
        }
    }

    static void lua_npc_ai(terraria_npc_handle_t *current, patch_handle_t inst) {
        LuaNpcEntry *e = npc_from_handle(current);
        if (!e || !inst || e->ai_ref == LUA_NOREF || !e->L) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->ai_ref);
        push_handle(L, inst);
        pcall_log(L, 1, 0, "mod.npc.ai");
    }

    static void lua_npc_on_death(terraria_npc_handle_t *current, patch_handle_t inst) {
        LuaNpcEntry *e = npc_from_handle(current);
        if (!e || !inst) return;

        if (e->on_death_ref != LUA_NOREF && e->L) {
            lua_State *L = e->L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, e->on_death_ref);
            push_handle(L, inst);
            pcall_log(L, 1, 0, "mod.npc.on_death");
        }

        if (e->loot.empty()) return;
        float pos[2] = {0.0f, 0.0f};
        int w = 0, h = 0;
        patchlib_field_get_value(e->f_pos, inst, pos);
        patchlib_field_get_value(e->f_width, inst, &w);
        patchlib_field_get_value(e->f_height, inst, &h);
        const float cx = pos[0] + (float) w / 2.0f;
        const float cy = pos[1] + (float) h / 2.0f;
        LOG_DEBUG("[mod.npc] on_death: dropping {} loot entries at ({:.0f},{:.0f})",
                  e->loot.size(), cx, cy);
        for (const auto &l : e->loot)
            terraria_npc_manager_drop_item(l.item_id, cx, cy, l.count);
    }

    static patch_handle_t lua_npc_get_texture(terraria_npc_handle_t *current) {
        LuaNpcEntry *e = npc_from_handle(current);
        if (!e) return PATCH_NULL;
        if (!e->tex_tried) {
            e->tex_tried = true;
            if (!e->texture_path.empty() &&
                !image::decode_png_file(e->texture_path, e->tex_pixels, e->tex_w, e->tex_h))
                LOG_WARN("[mod.npc] 贴图解码失败: {}", e->texture_path);
        }
        if (e->tex_pixels.empty() || e->tex_w <= 0 || e->tex_h <= 0) return PATCH_NULL;

        const size_t w = static_cast<size_t>(e->tex_w), h = static_cast<size_t>(e->tex_h);
        const size_t sz = w * h * 4;
        auto *buf = static_cast<unsigned char *>(malloc(sz));
        if (!buf) return PATCH_NULL;
#if defined(__ANDROID__)
        for (size_t y = 0; y < h; ++y)
            std::memcpy(buf + y * w * 4, e->tex_pixels.data() + (h - 1 - y) * w * 4, w * 4);
#else
        std::memcpy(buf, e->tex_pixels.data(), sz);
#endif
        premultiply_alpha(buf, sz);
        const patch_handle_t tex =
                terraria_texture2d_create(e->tex_w, e->tex_h, TEXTURE_FORMAT_RGBA32, buf, sz);
        free(buf);
        if (!patchlib_is_valid(tex)) return PATCH_NULL;

        const patch_handle_t tex_type = terraria_texture2d_get_class();
        const patch_handle_t asset = terraria_asset_create(tex_type, tex);
        if (patchlib_is_valid(tex_type)) patchlib_free(tex_type);
        if (patchlib_is_valid(tex)) patchlib_free(tex);
        return asset;
    }

    static int l_npc_register(lua_State *L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_mod_handle_t *handle = get_handle(L);
        if (!handle) return luaL_error(L, "mod handle missing");

        lua_getfield(L, 1, "name");
        const char *name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        if (!name || !*name) {
            lua_pop(L, 1);
            return luaL_error(L, "mod.npc.register: 缺少 name");
        }
        const std::string npc_name = name;
        lua_pop(L, 1);

        auto *e = new LuaNpcEntry();
        std::memset(&e->npc, 0, sizeof(e->npc));
        e->owner = handle;
        e->L = L;
        e->name = npc_name;
        e->defaults_ref = LUA_NOREF;
        e->ai_ref = LUA_NOREF;
        e->on_death_ref = LUA_NOREF;
        e->f_pos = PATCH_NULL;
        e->f_width = PATCH_NULL;
        e->f_height = PATCH_NULL;
        e->tex_w = e->tex_h = 0;
        e->tex_tried = false;
        e->registered = false;
        e->runtime_id = -1;

        // vanilla_ai：true = 原版 AI 先执行，再调用 ai
        lua_getfield(L, 1, "vanilla_ai");
        const bool vanilla_ai = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 1, "frames");
        e->npc.frames = static_cast<int>(luaL_optinteger(L, -1, 0));
        lua_pop(L, 1);
        if (e->npc.frames < 0) e->npc.frames = 0;

        // defaults：表格或函数
        lua_getfield(L, 1, "defaults");
        if (lua_isfunction(L, -1)) {
            e->defaults_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            if (lua_istable(L, -1)) {
                const patch_handle_t npc_class = patchlib_type_get_type("Terraria", "NPC");
                if (patchlib_is_valid(npc_class)) {
                    lua_pushnil(L);
                    while (lua_next(L, -2)) {
                        if (lua_type(L, -2) == LUA_TSTRING) {
                            const char *fname = lua_tostring(L, -2);
                            const patch_handle_t f = patchlib_type_get_field(npc_class, fname);
                            if (patchlib_is_valid(f)) {
                                const patch_type_t t = patchlib_field_get_type(f);
                                uint64_t v = 0;
                                if (try_read_patch_value(L, -1, t, &v))
                                    e->defaults.push_back({f, t, v, ref_default_value(L, -1, t)});
                                else
                                    LOG_WARN("[mod.npc] {}: 字段 {} 值类型不匹配，已忽略",
                                             npc_name, fname);
                            } else {
                                LOG_WARN("[mod.npc] {}: 找不到 NPC 字段 {}",
                                         npc_name, fname);
                            }
                        }
                        lua_pop(L, 1);
                    }
                }
                patchlib_free(npc_class);
            }
            lua_pop(L, 1);
        }

        lua_getfield(L, 1, "ai");
        if (lua_isfunction(L, -1))
            e->ai_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else
            lua_pop(L, 1);

        lua_getfield(L, 1, "on_death");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "on_kill"); }
        if (lua_isfunction(L, -1)) e->on_death_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        lua_getfield(L, 1, "loot");
        if (lua_istable(L, -1)) {
            const int n = (int) lua_rawlen(L, -1);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_istable(L, -1)) {
                    LuaNpcLoot l;
                    lua_getfield(L, -1, "id");
                    l.item_id = (int) luaL_optinteger(L, -1, 0);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "count");
                    l.count = (int) luaL_optinteger(L, -1, 1);
                    lua_pop(L, 1);
                    if (l.item_id > 0) e->loot.push_back(l);
                    else LOG_WARN("[mod.npc] {}: loot 需要数字 id", npc_name);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "texture");
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *rel = lua_tostring(L, -1);
            e->texture_path = resolve_item_image(handle->private_dir, handle->mod_dir, rel);
            if (e->texture_path.empty())
                LOG_WARN("[mod.npc] {}: 未找到贴图 {}", npc_name, rel);
        }
        lua_pop(L, 1);

        e->npc.parent_modloader_id = LUALOADER_PKG_ID;
        e->npc.parent_id = handle->mod_id.c_str();
        e->npc.internal_name = e->name.c_str();
        e->npc.runtime_id = -1;
        e->npc.ai_after_vanilla = vanilla_ai;
        e->npc.npc_ops.early_init = nullptr;
        e->npc.npc_ops.init_static = nullptr;
        e->npc.npc_ops.set_defaults = lua_npc_set_defaults;
        e->npc.npc_ops.ai = (e->ai_ref != LUA_NOREF) ? lua_npc_ai : nullptr;
        e->npc.npc_ops.get_texture = lua_npc_get_texture;
        e->npc.npc_ops.on_death = (e->on_death_ref != LUA_NOREF)
                ? lua_npc_on_death : nullptr;
        if (!e->loot.empty())
            e->npc.npc_ops.on_death = lua_npc_on_death;

        patch_handle_t npc_cls = patchlib_type_get_type("Terraria", "NPC");
        e->f_pos = patchlib_type_get_field(npc_cls, "position");
        e->f_width = patchlib_type_get_field(npc_cls, "width");
        e->f_height = patchlib_type_get_field(npc_cls, "height");
        patchlib_free(npc_cls);

        if (!terraria_npc_manager_register_npc(&e->npc)) {
            LOG_ERROR("[mod.npc] {}: 注册失败", npc_name);
            if (e->defaults_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->defaults_ref);
            if (e->ai_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->ai_ref);
            if (e->on_death_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->on_death_ref);
            unref_default_values(L, e->defaults);
            delete e;
            lua_pushnil(L);
            return 1;
        }

        e->registered = true;
        e->runtime_id = e->npc.runtime_id;
        g_lua_npcs.push_back(e);
        LOG_INFO("[mod.npc] 已注册 {} (runtime_id={})", npc_name, e->runtime_id);
        lua_pushinteger(L, e->runtime_id);
        return 1;
    }

    static int l_npc_runtime_id(lua_State *L) {
        const char *name = luaL_checkstring(L, 1);
        lua_mod_handle_t *handle = get_handle(L);
        for (LuaNpcEntry *e: g_lua_npcs) {
            if (e->owner == handle && e->name == name) {
                if (e->npc.runtime_id >= 0) lua_pushinteger(L, e->npc.runtime_id);
                else lua_pushnil(L);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    static int l_npc_spawn(lua_State *L) {
        const char *name = luaL_checkstring(L, 1);
        const float x = static_cast<float>(luaL_checknumber(L, 2));
        const float y = static_cast<float>(luaL_checknumber(L, 3));
        lua_mod_handle_t *handle = get_handle(L);
        for (LuaNpcEntry *e: g_lua_npcs) {
            if (e->owner == handle && e->name == name) {
                if (e->npc.runtime_id < 0) break;
                const int index = terraria_npc_manager_spawn(e->npc.runtime_id, x, y);
                if (index >= 0) lua_pushinteger(L, index);
                else lua_pushnil(L);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    static int l_npc_drop_item(lua_State *L) {
        const int id = (int) luaL_checkinteger(L, 1);
        const float x = (float) luaL_checknumber(L, 2);
        const float y = (float) luaL_checknumber(L, 3);
        const int count = (int) luaL_optinteger(L, 4, 1);
        const int index = terraria_npc_manager_drop_item(id, x, y, count);
        if (index >= 0) lua_pushinteger(L, index);
        else lua_pushnil(L);
        return 1;
    }

    static const luaL_Reg mod_npc_functions[] = {
            {"register", l_npc_register},
            {"runtime_id", l_npc_runtime_id},
            {"spawn", l_npc_spawn},
            {"drop_item", l_npc_drop_item},
            {nullptr, nullptr},
    };

    void unregister_npcs(lua_mod_handle_t *handle) {
        if (!handle) return;
        for (size_t i = g_lua_npcs.size(); i-- > 0;) {
            LuaNpcEntry *e = g_lua_npcs[i];
            if (e->owner != handle) continue;
            if (e->registered)
                terraria_npc_manager_unregister_npc(&e->npc);
            if (handle->L) {
                if (e->defaults_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->defaults_ref);
                if (e->ai_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->ai_ref);
                if (e->on_death_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->on_death_ref);
                unref_default_values(handle->L, e->defaults);
            }
            delete e;
            g_lua_npcs.erase(g_lua_npcs.begin() + static_cast<long>(i));
        }
    }

    void add_npc(lua_State *L) {
        luaL_setfuncs(L, mod_npc_functions, 0);
    }

} // namespace lualoader::lua_api
