#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ========================================================================
    // mod.buff：Lua 侧注册自定义 Buff
    //
    //   mod.buff.register{
    //     name        = "test_buff",      -- 内部名（同 Mod 内唯一，必填）
    //     texture     = "buff.png",       -- 图标 PNG，相对 Resources/Image 或 lib/Image
    //     debuff      = false,            -- 减益（图标红框、优先被顶掉）
    //     no_time     = false,            -- 不显示剩余时间
    //     pvp         = false,            -- 仅 PvP 生效
    //     light_pet   = false,            -- 光源宠物（同类互斥）
    //     vanity_pet  = false,            -- 观赏宠物（同类互斥）
    //     apply       = function(player, time) end,      -- 挂上时
    //     update      = function(player, buff_index) end,-- 每帧（激活时；buff_index 为内核激活列表下标）
    //     language    = { en = {name=, description=}, zh = {...} },
    //   } -> runtime_id | nil
    //
    // 说明：自定义 Buff 不写入 Player.buffType，原版按 buff id 定长的表不需要扩容；
    //       存档由内核的 plr.tef 伴生文件按稳定键存读。
    // ========================================================================

    /// 内核以 terraria_buff_handle_t* 回调，故 buff 必须是首成员。
    struct LuaBuffEntry {
        terraria_buff_handle_t buff;
        lua_mod_handle_t *owner;
        lua_State *L;
        std::string name;
        int apply_ref;
        int update_ref;
        std::string texture_path;
        std::vector<unsigned char> tex_pixels;
        int tex_w, tex_h;
        bool tex_tried;
        bool registered;
        int runtime_id;
        std::map<int, std::pair<std::string, std::string> > lang; // culture -> {name, description}
    };

    static std::vector<LuaBuffEntry *> g_lua_buffs;
    static std::map<std::string, std::string> g_lua_buff_lang;
    static bool g_buff_lang_hooked = false;

    static LuaBuffEntry *buff_from_handle(terraria_buff_handle_t *h) {
        return reinterpret_cast<LuaBuffEntry *>(h);
    }

    static void lua_buff_apply(terraria_buff_handle_t *current, patch_handle_t player, int time) {
        LuaBuffEntry *e = buff_from_handle(current);
        if (!e || !e->L || e->apply_ref == LUA_NOREF) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->apply_ref);
        push_handle(L, player);
        lua_pushinteger(L, time);
        pcall_log(L, 2, 0, "mod.buff.apply");
    }

    static void lua_buff_update(terraria_buff_handle_t *current, patch_handle_t player,
                                int buff_index) {
        LuaBuffEntry *e = buff_from_handle(current);
        if (!e || !e->L || e->update_ref == LUA_NOREF) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->update_ref);
        push_handle(L, player);
        lua_pushinteger(L, buff_index);
        pcall_log(L, 2, 0, "mod.buff.update");
    }

    static patch_handle_t lua_buff_get_texture(terraria_buff_handle_t *current) {
        LuaBuffEntry *e = buff_from_handle(current);
        if (!e) return PATCH_NULL;
        if (!e->tex_tried) {
            e->tex_tried = true;
            if (!e->texture_path.empty() &&
                !image::decode_png_file(e->texture_path, e->tex_pixels, e->tex_w, e->tex_h))
                LOG_WARN("[mod.buff] 贴图解码失败: {}", e->texture_path);
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

        /* 内核现在自己画图标，直接给 Texture2D（不再包 Asset） */
        return tex;
    }

    // ---- 本地化 ----

    static int culture_from_key(const char *key) {
        if (!key) return 0;
        std::string s(key);
        for (char &ch: s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (s == "en" || s == "english") return LANGUAGE_ENGLISH;
        if (s == "de" || s == "german") return LANGUAGE_GERMAN;
        if (s == "it" || s == "italian") return LANGUAGE_ITALIAN;
        if (s == "fr" || s == "french") return LANGUAGE_FRENCH;
        if (s == "es" || s == "spanish") return LANGUAGE_SPANISH;
        if (s == "ru" || s == "russian") return LANGUAGE_RUSSIAN;
        if (s == "zh" || s == "zh-cn" || s == "zh-hans" || s == "chinese" ||
            s == "chinesesimplified")
            return LANGUAGE_CHINESE_SIMPLIFIED;
        if (s == "pt" || s == "portuguese") return LANGUAGE_PORTUGUESE;
        if (s == "pl" || s == "polish") return LANGUAGE_POLISH;
        if (s == "ja" || s == "japanese") return LANGUAGE_JAPANESE;
        if (s == "ko" || s == "korean") return LANGUAGE_KOREAN;
        if (s == "zh-tw" || s == "zh-hant" || s == "chinesetraditional")
            return LANGUAGE_CHINESE_TRADITIONAL;
        return 0;
    }

    static std::string json_escape(const std::string &in) {
        std::string o;
        o.reserve(in.size() + 8);
        for (char ch: in) {
            const auto c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof b, "\\u%04x", c);
                        o += b;
                    } else o += static_cast<char>(c);
            }
        }
        return o;
    }

    /// 重建每种语言的 JSON：{ "buff_name_<loader>_<mod>": { "<name>": "..." }, ... }
    static void rebuild_buff_language_cache() {
        g_lua_buff_lang.clear();
        typedef std::vector<std::pair<std::string, std::string> > EntryList;
        for (int c = 1; c <= 12; ++c) {
            std::map<std::string, EntryList> names, descs;
            for (LuaBuffEntry *e: g_lua_buffs) {
                const auto it = e->lang.find(c);
                if (it == e->lang.end()) continue;
                const std::string mod = e->owner ? e->owner->mod_id : std::string("?");
                if (!it->second.first.empty()) names[mod].emplace_back(e->name, it->second.first);
                if (!it->second.second.empty()) descs[mod].emplace_back(e->name, it->second.second);
            }
            std::string json = "{";
            bool first = true;
            const auto emit = [&](const char *prefix, const std::map<std::string, EntryList> &m) {
                for (const auto &kv: m) {
                    if (!first) json += ",";
                    first = false;
                    json += "\"";
                    json += prefix;
                    json += "_";
                    json += LUALOADER_PKG_ID;
                    json += "_";
                    json += json_escape(kv.first);
                    json += "\":{";
                    bool f2 = true;
                    for (const auto &p: kv.second) {
                        if (!f2) json += ",";
                        f2 = false;
                        json += "\"";
                        json += json_escape(p.first);
                        json += "\":\"";
                        json += json_escape(p.second);
                        json += "\"";
                    }
                    json += "}";
                }
            };
            emit("buff_name", names);
            emit("buff_description", descs);
            json += "}";
            g_lua_buff_lang[std::to_string(c)] = std::move(json);
        }
    }

    // 供内核显示 tooltip：按当前语言取名字/说明（UTF-8，生命周期由 entry 保证）
    static int g_display_culture = 0;

    /// 读游戏当前语言（Terraria.Localization.Language.ActiveCulture）
    static int current_culture() {
        const patch_handle_t cls = patchlib_type_get_type("Terraria.Localization", "Language");
        if (!cls) return 0;
        const patch_handle_t f = patchlib_type_get_field(cls, "ActiveCulture");
        int c = 0;
        if (f) patchlib_field_get_value(f, nullptr, &c);
        patchlib_free(f);
        patchlib_free(cls);
        return c;
    }

    static const char *lua_buff_display_name(terraria_buff_handle_t *current) {
        LuaBuffEntry *e = buff_from_handle(current);
        if (!e) return nullptr;
        int culture = g_display_culture;
        if (culture <= 0) culture = current_culture();
        const auto it = e->lang.find(culture);
        if (it != e->lang.end() && !it->second.first.empty())
            return it->second.first.c_str();
        if (!e->lang.empty() && !e->lang.begin()->second.first.empty())
            return e->lang.begin()->second.first.c_str();
        return e->name.c_str();
    }

    static const char *lua_buff_display_description(terraria_buff_handle_t *current) {
        LuaBuffEntry *e = buff_from_handle(current);
        if (!e) return nullptr;
        int culture = g_display_culture;
        if (culture <= 0) culture = current_culture();
        const auto it = e->lang.find(culture);
        if (it != e->lang.end() && !it->second.second.empty())
            return it->second.second.c_str();
        if (!e->lang.empty() && !e->lang.begin()->second.second.empty())
            return e->lang.begin()->second.second.c_str();
        return nullptr;
    }

    static const char *lua_buff_language(const language_culture_t culture) {
        if (static_cast<int>(culture) > 0) {
            const bool changed = g_display_culture != static_cast<int>(culture);
            g_display_culture = static_cast<int>(culture);
            /* 语言确定后，把已注册 Buff 的显示文本重新拷进内核 */
            if (changed) {
                for (LuaBuffEntry *e: g_lua_buffs)
                    if (e && e->registered)
                        terraria_buff_manager_refresh_display_text(&e->buff);
            }
        }
        const auto it = g_lua_buff_lang.find(std::to_string(static_cast<int>(culture)));
        return it == g_lua_buff_lang.end() ? "{}" : it->second.c_str();
    }

    static void ensure_buff_language_hook() {
        if (g_buff_lang_hooked) return;
        g_buff_lang_hooked = true;
        terraria_language_manager_register_callback(lua_buff_language);
    }

    void shutdown_buff_hooks() {
        if (!g_buff_lang_hooked) return;
        if (terraria_language_manager_unregister_callback)
            terraria_language_manager_unregister_callback(lua_buff_language);
        g_buff_lang_hooked = false;
    }

    // ---- register ----

    static int l_buff_register(lua_State *L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_mod_handle_t *handle = get_handle(L);
        if (!handle) return luaL_error(L, "mod handle missing");

        lua_getfield(L, 1, "name");
        const char *name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        if (!name || !*name) {
            lua_pop(L, 1);
            return luaL_error(L, "mod.buff.register: 缺少 name");
        }
        const std::string buff_name = name;
        lua_pop(L, 1);

        auto *e = new LuaBuffEntry();
        std::memset(&e->buff, 0, sizeof(e->buff));
        e->owner = handle;
        e->L = L;
        e->name = buff_name;
        e->apply_ref = LUA_NOREF;
        e->update_ref = LUA_NOREF;
        e->tex_w = e->tex_h = 0;
        e->tex_tried = false;
        e->registered = false;
        e->runtime_id = -1;

        lua_getfield(L, 1, "debuff");
        e->buff.debuff = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 1, "no_time");
        e->buff.no_time_display = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 1, "pvp");
        e->buff.pvp = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 1, "light_pet");
        e->buff.light_pet = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 1, "vanity_pet");
        e->buff.vanity_pet = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        // apply / update 回调
        lua_getfield(L, 1, "apply");
        if (lua_isfunction(L, -1)) e->apply_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        lua_getfield(L, 1, "update");
        if (lua_isfunction(L, -1)) e->update_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        // language：{ en = {name=, description=}, zh = {...} }
        lua_getfield(L, 1, "language");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                int culture = 0;
                if (lua_type(L, -2) == LUA_TSTRING) culture = culture_from_key(lua_tostring(L, -2));
                else if (lua_isnumber(L, -2)) culture = static_cast<int>(lua_tointeger(L, -2));
                if (culture > 0 && lua_istable(L, -1)) {
                    std::string dn, dd;
                    lua_getfield(L, -1, "name");
                    if (lua_type(L, -1) == LUA_TSTRING) dn = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "description");
                    if (lua_type(L, -1) == LUA_TSTRING) dd = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    e->lang[culture] = std::make_pair(dn, dd);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        // texture
        lua_getfield(L, 1, "texture");
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *rel = lua_tostring(L, -1);
            e->texture_path = resolve_item_image(handle->private_dir, handle->mod_dir, rel);
            if (e->texture_path.empty())
                LOG_WARN("[mod.buff] {}: 未找到贴图 {}", buff_name, rel);
        }
        lua_pop(L, 1);

        // 组装内核句柄
        e->buff.parent_modloader_id = LUALOADER_PKG_ID;
        e->buff.parent_id = handle->mod_id.c_str();
        e->buff.internal_name = e->name.c_str();
        e->buff.runtime_id = -1;
        e->buff.buff_ops.early_init = nullptr;
        e->buff.buff_ops.init_static = nullptr;
        e->buff.buff_ops.apply = (e->apply_ref != LUA_NOREF) ? lua_buff_apply : nullptr;
        e->buff.buff_ops.update = (e->update_ref != LUA_NOREF) ? lua_buff_update : nullptr;
        e->buff.display_name = nullptr;
        e->buff.display_description = nullptr;
        e->buff.buff_ops.get_texture = lua_buff_get_texture;
        e->buff.buff_ops.get_display_name = lua_buff_display_name;
        e->buff.buff_ops.get_display_description = lua_buff_display_description;

        if (!terraria_buff_manager_register_buff(&e->buff)) {
            LOG_ERROR("[mod.buff] {}: 注册失败", buff_name);
            if (e->apply_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->apply_ref);
            if (e->update_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->update_ref);
            delete e;
            lua_pushnil(L);
            return 1;
        }

        e->registered = true;
        e->runtime_id = e->buff.runtime_id;
        g_lua_buffs.push_back(e);
        rebuild_buff_language_cache();
        ensure_buff_language_hook();
        LOG_INFO("[mod.buff] 已注册 {} (runtime_id={})", buff_name, e->runtime_id);
        lua_pushinteger(L, e->runtime_id);
        return 1;
    }

    static int l_buff_runtime_id(lua_State *L) {
        const char *name = luaL_checkstring(L, 1);
        lua_mod_handle_t *handle = get_handle(L);
        for (LuaBuffEntry *e: g_lua_buffs) {
            if (e->owner == handle && e->name == name) {
                if (e->buff.runtime_id >= 0) lua_pushinteger(L, e->buff.runtime_id);
                else lua_pushnil(L);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    static int l_buff_add(lua_State *L) {
        const int id = static_cast<int>(luaL_checkinteger(L, 1));
        const patch_handle_t player = to_handle(L, 2);
        const int time = static_cast<int>(luaL_checkinteger(L, 3));
        terraria_buff_handle_t *h = terraria_buff_manager_get_buff(id);
        if (!h || !player) {
            lua_pushinteger(L, -1);
            return 1;
        }
        lua_pushinteger(L, terraria_buff_manager_add(h, player, time));
        return 1;
    }

    static int l_buff_time(lua_State *L) {
        const int id = static_cast<int>(luaL_checkinteger(L, 1));
        const patch_handle_t player = to_handle(L, 2);
        terraria_buff_handle_t *h = terraria_buff_manager_get_buff(id);
        lua_pushinteger(L, (h && player) ? terraria_buff_manager_time(h, player) : 0);
        return 1;
    }

    static int l_buff_remove(lua_State *L) {
        const int id = static_cast<int>(luaL_checkinteger(L, 1));
        const patch_handle_t player = to_handle(L, 2);
        terraria_buff_handle_t *h = terraria_buff_manager_get_buff(id);
        if (h && player) terraria_buff_manager_remove(h, player);
        return 0;
    }

    static const luaL_Reg mod_buff_functions[] = {
            {"register", l_buff_register},
            {"runtime_id", l_buff_runtime_id},
            {"add", l_buff_add},
            {"time", l_buff_time},
            {"remove", l_buff_remove},
            {nullptr, nullptr},
    };

    void unregister_buffs(lua_mod_handle_t *handle) {
        if (!handle) return;
        bool changed = false;
        for (size_t i = g_lua_buffs.size(); i-- > 0;) {
            LuaBuffEntry *e = g_lua_buffs[i];
            if (e->owner != handle) continue;
            if (e->registered)
                terraria_buff_manager_unregister_buff(&e->buff);
            if (handle->L) {
                if (e->apply_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->apply_ref);
                if (e->update_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->update_ref);
            }
            delete e;
            g_lua_buffs.erase(g_lua_buffs.begin() + static_cast<long>(i));
            changed = true;
        }
        if (changed) rebuild_buff_language_cache();
    }

    void add_buff(lua_State *L) {
        luaL_setfuncs(L, mod_buff_functions, 0);
    }

} // namespace lualoader::lua_api
