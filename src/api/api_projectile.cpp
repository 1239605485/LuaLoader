#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ========================================================================
    // mod.projectile：Lua 侧注册自定义弹幕
    //
    //   mod.projectile.register{
    //     name      = "test_orb",           -- 内部名（同 Mod 内唯一，必填）
    //     texture   = "orb.png",            -- PNG，相对 Resources/Image 或 lib/Image
    //     no_ai     = false,                -- true = 不接管 AI（沿用原版 aiStyle）
    //     defaults  = { damage=10, ... },   -- 或 function(projectile) end
    //     ai        = function(projectile) end,  -- 每帧；装了 ai 则跳过原版 AI
    //   } -> runtime_id | nil
    // ========================================================================

    /// 内核以 terraria_projectile_handle_t* 回调，故 projectile 必须是首成员。
    struct LuaProjectileEntry {
        terraria_projectile_handle_t projectile;
        lua_mod_handle_t *owner;
        lua_State *L;
        std::string name;
        std::vector<LuaItemFieldValue> defaults;
        int defaults_ref;
        int ai_ref;
        int on_kill_ref;
        int hit_npc_ref;
        int hit_player_ref;
        int tile_collide_ref;
        std::string texture_path;
        std::vector<unsigned char> tex_pixels;
        int tex_w, tex_h;
        bool tex_tried;
        bool registered;
        int runtime_id;
    };

    static std::vector<LuaProjectileEntry *> g_lua_projectiles;

    static LuaProjectileEntry *projectile_from_handle(terraria_projectile_handle_t *h) {
        return reinterpret_cast<LuaProjectileEntry *>(h);
    }

    static void lua_projectile_set_defaults(terraria_projectile_handle_t *current,
                                            const patch_handle_t inst) {
        LuaProjectileEntry *e = projectile_from_handle(current);
        if (!e || !inst) return;
        for (const auto &d: e->defaults)
            if (patchlib_is_valid(d.field)) item_field_write(d.field, inst, d.type, &d.value);
        if (e->defaults_ref != LUA_NOREF && e->L) {
            lua_State *L = e->L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, e->defaults_ref);
            push_handle(L, inst);
            pcall_log(L, 1, 0, "mod.projectile.defaults");
        }
    }

    static void lua_projectile_ai(terraria_projectile_handle_t *current,
                                  const patch_handle_t inst) {
        LuaProjectileEntry *e = projectile_from_handle(current);
        if (!e || !inst || e->ai_ref == LUA_NOREF || !e->L) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->ai_ref);
        push_handle(L, inst);
        pcall_log(L, 1, 0, "mod.projectile.ai");
    }

    static void lua_projectile_on_kill(terraria_projectile_handle_t *current,
                                       const patch_handle_t inst) {
        LuaProjectileEntry *e = projectile_from_handle(current);
        if (!e || !inst || e->on_kill_ref == LUA_NOREF || !e->L) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->on_kill_ref);
        push_handle(L, inst);
        pcall_log(L, 1, 0, "mod.projectile.on_kill");
    }

    static void lua_projectile_hit_npc(terraria_projectile_handle_t *current,
                                       const patch_handle_t inst, const patch_handle_t npc,
                                       const int damage, const int hit_direction,
                                       const bool crit) {
        LuaProjectileEntry *e = projectile_from_handle(current);
        if (!e || !inst || e->hit_npc_ref == LUA_NOREF || !e->L) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->hit_npc_ref);
        push_handle(L, inst);
        push_handle(L, npc);
        lua_pushinteger(L, damage);
        lua_pushinteger(L, hit_direction);
        lua_pushboolean(L, crit);
        pcall_log(L, 5, 0, "mod.projectile.on_hit_npc");
    }

    static void lua_projectile_hit_player(terraria_projectile_handle_t *current,
                                          const patch_handle_t inst, const patch_handle_t player,
                                          const int damage, const int hit_direction,
                                          const bool pvp, const bool crit) {
        LuaProjectileEntry *e = projectile_from_handle(current);
        if (!e || !inst || e->hit_player_ref == LUA_NOREF || !e->L) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->hit_player_ref);
        push_handle(L, inst);
        push_handle(L, player);
        lua_pushinteger(L, damage);
        lua_pushinteger(L, hit_direction);
        lua_pushboolean(L, pvp);
        lua_pushboolean(L, crit);
        pcall_log(L, 6, 0, "mod.projectile.on_hit_player");
    }

    static void lua_projectile_tile_collide(terraria_projectile_handle_t *current,
                                            const patch_handle_t inst,
                                            const float old_vx, const float old_vy) {
        LuaProjectileEntry *e = projectile_from_handle(current);
        if (!e || !inst || e->tile_collide_ref == LUA_NOREF || !e->L) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->tile_collide_ref);
        push_handle(L, inst);
        lua_pushnumber(L, old_vx);
        lua_pushnumber(L, old_vy);
        pcall_log(L, 3, 0, "mod.projectile.on_tile_collide");
    }

    static patch_handle_t lua_projectile_get_texture(terraria_projectile_handle_t *current) {
        LuaProjectileEntry *e = projectile_from_handle(current);
        if (!e) return PATCH_NULL;
        if (!e->tex_tried) {
            e->tex_tried = true;
            if (!e->texture_path.empty() &&
                !image::decode_png_file(e->texture_path, e->tex_pixels, e->tex_w, e->tex_h))
                LOG_WARN("[mod.projectile] 贴图解码失败: {}", e->texture_path);
        }
        if (e->tex_pixels.empty() || e->tex_w <= 0 || e->tex_h <= 0) return PATCH_NULL;

        const size_t w = static_cast<size_t>(e->tex_w), h = static_cast<size_t>(e->tex_h);
        const size_t sz = w * h * 4;
        auto *buf = static_cast<unsigned char *>(malloc(sz));
        if (!buf) return PATCH_NULL;
#if defined(__ANDROID__)
        // Android 纹理坐标系需 Y 轴翻转
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

        // TextureAssets.Projectile[] 存的是 Asset<Texture2D>，必须包一层
        const patch_handle_t tex_type = terraria_texture2d_get_class();
        const patch_handle_t asset = terraria_asset_create(tex_type, tex);
        if (patchlib_is_valid(tex_type)) patchlib_free(tex_type);
        if (patchlib_is_valid(tex)) patchlib_free(tex);
        return asset;
    }

    static int l_projectile_register(lua_State *L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_mod_handle_t *handle = get_handle(L);
        if (!handle) return luaL_error(L, "mod handle missing");

        lua_getfield(L, 1, "name");
        const char *name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        if (!name || !*name) {
            lua_pop(L, 1);
            return luaL_error(L, "mod.projectile.register: 缺少 name");
        }
        const std::string proj_name = name;
        lua_pop(L, 1);

        auto *e = new LuaProjectileEntry();
        std::memset(&e->projectile, 0, sizeof(e->projectile));
        e->owner = handle;
        e->L = L;
        e->name = proj_name;
        e->defaults_ref = LUA_NOREF;
        e->ai_ref = LUA_NOREF;
        e->on_kill_ref = LUA_NOREF;
        e->hit_npc_ref = LUA_NOREF;
        e->hit_player_ref = LUA_NOREF;
        e->tile_collide_ref = LUA_NOREF;
        e->tex_w = e->tex_h = 0;
        e->tex_tried = false;
        e->registered = false;
        e->runtime_id = -1;

        // no_ai：true = 不接管 AI，沿用原版 aiStyle
        lua_getfield(L, 1, "no_ai");
        const bool no_ai = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        // vanilla_ai：true = 原版 AI 先执行，再调用 ai 回调
        lua_getfield(L, 1, "vanilla_ai");
        const bool vanilla_ai = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        // frames：竖向帧图帧数（写进 Main.projFrames[type]；0 = 单帧整图）
        lua_getfield(L, 1, "frames");
        e->projectile.frames = static_cast<int>(luaL_optinteger(L, -1, 0));
        lua_pop(L, 1);
        if (e->projectile.frames < 0) e->projectile.frames = 0;

        // defaults：表格（声明式）或函数
        lua_getfield(L, 1, "defaults");
        if (lua_isfunction(L, -1)) {
            e->defaults_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            if (lua_istable(L, -1)) {
                const patch_handle_t proj_class =
                        patchlib_type_get_type("Terraria", "Projectile");
                if (patchlib_is_valid(proj_class)) {
                    lua_pushnil(L);
                    while (lua_next(L, -2)) {
                        if (lua_type(L, -2) == LUA_TSTRING) {
                            const char *fname = lua_tostring(L, -2);
                            const patch_handle_t f = patchlib_type_get_field(proj_class, fname);
                            if (patchlib_is_valid(f)) {
                                const patch_type_t t = patchlib_field_get_type(f);
                                uint64_t v = 0;
                                if (try_read_patch_value(L, -1, t, &v)) {
                                    e->defaults.push_back({f, t, v, ref_default_value(L, -1, t)});
                                } else {
                                    LOG_WARN("[mod.projectile] {}: 字段 {} 值类型不匹配，已忽略",
                                             proj_name, fname);
                                }
                            } else {
                                patchlib_free(f);
                                LOG_WARN("[mod.projectile] {}: 找不到 Projectile 字段 {}",
                                         proj_name, fname);
                            }
                        }
                        lua_pop(L, 1);
                    }
                }
                /* field 句柄已存进 defaults；桌面端不能释放其 class */
            }
            lua_pop(L, 1);
        }

        // ai：每帧回调（返回后内核跳过原版 AI）
        lua_getfield(L, 1, "ai");
        if (lua_isfunction(L, -1)) {
            e->ai_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
        lua_getfield(L, 1, "on_kill");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "kill"); }
        if (lua_isfunction(L, -1)) e->on_kill_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        lua_getfield(L, 1, "on_hit_npc");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "hit_npc"); }
        if (lua_isfunction(L, -1)) e->hit_npc_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        lua_getfield(L, 1, "on_hit_player");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "hit_player"); }
        if (lua_isfunction(L, -1)) e->hit_player_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        lua_getfield(L, 1, "on_tile_collide");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "tile_collide"); }
        if (lua_isfunction(L, -1)) e->tile_collide_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        if (no_ai && e->ai_ref != LUA_NOREF)
            LOG_WARN("[mod.projectile] {}: 同时给了 no_ai 和 ai，ai 不会生效", proj_name);
        if (no_ai && vanilla_ai)
            LOG_WARN("[mod.projectile] {}: no_ai 与 vanilla_ai 冲突，已按 no_ai 处理", proj_name);

        // texture
        lua_getfield(L, 1, "texture");
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *rel = lua_tostring(L, -1);
            e->texture_path = resolve_item_image(handle->private_dir, handle->mod_dir, rel);
            if (e->texture_path.empty())
                LOG_WARN("[mod.projectile] {}: 未找到贴图 {}", proj_name, rel);
        }
        lua_pop(L, 1);

        // 组装内核句柄
        e->projectile.parent_modloader_id = LUALOADER_PKG_ID;
        e->projectile.parent_id = handle->mod_id.c_str();
        e->projectile.internal_name = e->name.c_str();
        e->projectile.runtime_id = -1;
        e->projectile.no_ai_hook = no_ai;
        e->projectile.ai_after_vanilla = vanilla_ai;
        e->projectile.projectile_ops.early_init = nullptr;
        e->projectile.projectile_ops.init_static = nullptr;
        e->projectile.projectile_ops.set_defaults = lua_projectile_set_defaults;
        e->projectile.projectile_ops.ai = (e->ai_ref != LUA_NOREF) ? lua_projectile_ai : nullptr;
        e->projectile.projectile_ops.get_texture = lua_projectile_get_texture;
        e->projectile.projectile_ops.on_kill = (e->on_kill_ref != LUA_NOREF)
                ? lua_projectile_on_kill : nullptr;
        e->projectile.projectile_ops.on_hit_npc = (e->hit_npc_ref != LUA_NOREF)
                ? lua_projectile_hit_npc : nullptr;
        e->projectile.projectile_ops.on_hit_player = (e->hit_player_ref != LUA_NOREF)
                ? lua_projectile_hit_player : nullptr;
        e->projectile.projectile_ops.on_tile_collide = (e->tile_collide_ref != LUA_NOREF)
                ? lua_projectile_tile_collide : nullptr;

        if (!terraria_projectile_manager_register_projectile(&e->projectile)) {
            LOG_ERROR("[mod.projectile] {}: 注册失败", proj_name);
            if (e->defaults_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->defaults_ref);
            if (e->ai_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->ai_ref);
            if (e->on_kill_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->on_kill_ref);
            if (e->hit_npc_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->hit_npc_ref);
            if (e->hit_player_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->hit_player_ref);
            if (e->tile_collide_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->tile_collide_ref);
            unref_default_values(L, e->defaults);
            delete e;
            lua_pushnil(L);
            return 1;
        }

        e->registered = true;
        e->runtime_id = e->projectile.runtime_id;
        g_lua_projectiles.push_back(e);
        LOG_INFO("[mod.projectile] 已注册 {} (runtime_id={})", proj_name, e->runtime_id);
        lua_pushinteger(L, e->runtime_id);
        return 1;
    }

    static int l_projectile_runtime_id(lua_State *L) {
        const char *name = luaL_checkstring(L, 1);
        lua_mod_handle_t *handle = get_handle(L);
        for (LuaProjectileEntry *e: g_lua_projectiles) {
            if (e->owner == handle && e->name == name) {
                if (e->projectile.runtime_id >= 0) lua_pushinteger(L, e->projectile.runtime_id);
                else lua_pushnil(L);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    static const luaL_Reg mod_projectile_functions[] = {
            {"register", l_projectile_register},
            {"runtime_id", l_projectile_runtime_id},
            {nullptr, nullptr},
    };

    void unregister_projectiles(lua_mod_handle_t *handle) {
        if (!handle) return;
        for (size_t i = g_lua_projectiles.size(); i-- > 0;) {
            LuaProjectileEntry *e = g_lua_projectiles[i];
            if (e->owner != handle) continue;
            if (e->registered)
                terraria_projectile_manager_unregister_projectile(&e->projectile);
            if (handle->L) {
                if (e->defaults_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->defaults_ref);
                if (e->ai_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->ai_ref);
                if (e->on_kill_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->on_kill_ref);
                if (e->hit_npc_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->hit_npc_ref);
                if (e->hit_player_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->hit_player_ref);
                if (e->tile_collide_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->tile_collide_ref);
                unref_default_values(handle->L, e->defaults);
            }
            delete e;
            g_lua_projectiles.erase(g_lua_projectiles.begin() + static_cast<long>(i));
        }
    }


void add_projectile(lua_State *L) {
    luaL_setfuncs(L, mod_projectile_functions, 0);
}

} // namespace lualoader::lua_api
