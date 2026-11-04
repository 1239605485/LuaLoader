#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ========================================================================
    // mod.item：Lua 侧注册自定义物品
    //
    //   mod.item.register{
    //     name      = "test_sword",            -- 内部名（同 Mod 内唯一，必填）
    //     texture   = "sword.png",             -- PNG，相对 Resources/Image 或 lib/Image
    //     tooltip   = true,                    -- 是否显示简介
    //     sacrifice = 1,                       -- 研究所需数量（0 = 不可研究）
    //     defaults  = { damage=25, useTime=20, ... },   -- 或 function(item) end
    //     can_use   = function(player, item, ignore) return true end,
    //     language  = { en = {name=,tooltip=}, zh = {...} },
    //     recipe    = { result=1, materials={{id=9,count=10}}, station=18 },
    //   } -> runtime_id | nil
    // ========================================================================



    /// 内核以 terraria_item_handle_t* 回调，故 item 必须是首成员。
    struct LuaItemEntry {
        terraria_item_handle_t item;
        lua_mod_handle_t *owner;
        lua_State *L;
        std::string name;
        std::vector<LuaItemFieldValue> defaults;
        int defaults_ref;
        int can_use_ref;
        int use_ref;
        std::string texture_path;
        std::vector<unsigned char> tex_pixels;
        int tex_w, tex_h;
        bool tex_tried;
        int prefix_class;   // 0=不可重铸；见 mod.item.register 的 prefixes
        bool has_recipe;
        int recipe_stack;
        std::vector<std::pair<int, int> > recipe_materials;
        int recipe_station;
        std::map<int, std::pair<std::string, std::string> > lang;
        patch_handle_t use_sound;   // SoundID.ItemN 的 LegacySoundStyle 对象
        bool registered;
        int runtime_id;
    };

    static std::vector<LuaItemEntry *> g_lua_items;
    static std::map<int, std::string> g_lua_lang_cache;
    static bool g_lua_item_hooks_registered = false;

    static LuaItemEntry *item_from_handle(terraria_item_handle_t *h) {
        return reinterpret_cast<LuaItemEntry *>(h);
    }

    void item_field_write(const patch_handle_t field, const patch_handle_t inst,
                                 const patch_type_t type, const void *value) {
        const size_t size = patch_scalar_size(type);
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, inst);
        if (ptr) std::memcpy(ptr, value, size);
#else
        patchlib_field_set_value(field, inst, const_cast<void *>(value));
#endif
    }

    static void lua_item_set_defaults(terraria_item_handle_t *current, const patch_handle_t inst) {
        LuaItemEntry *e = item_from_handle(current);
        if (!e || !inst) return;
        for (const auto &d: e->defaults)
            if (patchlib_is_valid(d.field)) item_field_write(d.field, inst, d.type, &d.value);
        if (e->defaults_ref != LUA_NOREF && e->L) {
            lua_State *L = e->L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, e->defaults_ref);
            push_handle(L, inst);
            pcall_log(L, 1, 0, "mod.item.defaults"); // pcall_log already consumes errors
        }

        /* use_sound：每次 SetDefaults 都重新写，避免原版/其它 Mod 把它覆盖掉。
         * UseSound 是 LegacySoundStyle 引用字段，直接放 SoundID.ItemN 的对象。 */
        if (patchlib_is_valid(e->use_sound)) {
            static patch_handle_t f_use_sound = PATCH_NULL;
            static patch_handle_t item_cls = PATCH_NULL;
            if (!patchlib_is_valid(f_use_sound)) {
                item_cls = patchlib_type_get_type("Terraria", "Item");
                if (patchlib_is_valid(item_cls)) {
                    f_use_sound = patchlib_type_get_field(item_cls, "UseSound");
                    /* 桌面端 field 依附 class，class 必须常驻（不 free） */
                }
            }
            if (patchlib_is_valid(f_use_sound))
                patchlib_field_set_value(f_use_sound, inst, &e->use_sound);
        }
    }

    static void lua_item_use(terraria_item_handle_t *current, patch_handle_t player,
                             patch_handle_t item) {
        LuaItemEntry *e = item_from_handle(current);
        if (!e || !e->L || e->use_ref == LUA_NOREF) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->use_ref);
        push_handle(L, player);
        push_handle(L, item);
        pcall_log(L, 2, 0, "mod.item.use");
    }

    static bool lua_item_can_use(terraria_item_handle_t *current, const patch_handle_t player,
                                 const patch_handle_t item, const bool ignore_cursed) {
        LuaItemEntry *e = item_from_handle(current);
        if (!e || e->can_use_ref == LUA_NOREF || !e->L) return true;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->can_use_ref);
        push_handle(L, player);
        push_handle(L, item);
        lua_pushboolean(L, ignore_cursed);
        if (pcall_log(L, 3, 1, "mod.item.can_use") != LUA_OK) {
            return true;
        }
        const bool r = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return r;
    }

    /// PNG 是直通 alpha（straight alpha），而 Terraria 的混合状态按
    /// **预乘 alpha**（premultiplied，BlendState.AlphaBlend: SrcBlend=One）
    /// 处理。不预乘的话，半透明像素（光效/描边/阴影）会被画成不透明的
    /// 亮色。上传前把 RGB 乘以 alpha，与原版内容管线一致。
    void premultiply_alpha(unsigned char *buf, const size_t sz) {
        for (size_t i = 0; i + 3 < sz; i += 4) {
            const unsigned a = buf[i + 3];
            if (a < 255) {
                buf[i]     = static_cast<unsigned char>((buf[i]     * a + 127) / 255);
                buf[i + 1] = static_cast<unsigned char>((buf[i + 1] * a + 127) / 255);
                buf[i + 2] = static_cast<unsigned char>((buf[i + 2] * a + 127) / 255);
            }
        }
    }

    static patch_handle_t lua_item_get_texture(terraria_item_handle_t *current) {
        LuaItemEntry *e = item_from_handle(current);
        if (!e) return PATCH_NULL;
        if (!e->tex_tried) {
            e->tex_tried = true;
            if (!e->texture_path.empty() &&
                !image::decode_png_file(e->texture_path, e->tex_pixels, e->tex_w, e->tex_h))
                LOG_WARN("[mod.item] 贴图解码失败: {}", e->texture_path);
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

        const patch_handle_t tex_type = terraria_texture2d_get_class();
        const patch_handle_t asset = terraria_asset_create(tex_type, tex);
        if (patchlib_is_valid(tex_type)) patchlib_free(tex_type);
        if (patchlib_is_valid(tex)) patchlib_free(tex);
        return asset;
    }

    static std::string json_escape(const std::string &s) {
        std::string o;
        o.reserve(s.size() + 2);
        for (const unsigned char c: s) {
            switch (c) {
                case '"': o += "\\\"";
                    break;
                case '\\': o += "\\\\";
                    break;
                case '\n': o += "\\n";
                    break;
                case '\r': o += "\\r";
                    break;
                case '\t': o += "\\t";
                    break;
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

    /// 重建每种语言的 JSON 缓存：{ "item_name_<loader>_<mod>": { "<name>": "..." }, ... }
    /// 预生成后在回调里只读，避免在非 Lua 线程上调用 Lua。
    static void rebuild_language_cache() {
        g_lua_lang_cache.clear();
        typedef std::vector<std::pair<std::string, std::string> > EntryList;
        for (int c = 1; c <= 12; ++c) {
            std::map<std::string, EntryList> names, tips;
            for (LuaItemEntry *e: g_lua_items) {
                const auto it = e->lang.find(c);
                if (it == e->lang.end()) continue;
                const std::string mod = e->owner ? e->owner->mod_id : std::string("?");
                if (!it->second.first.empty()) names[mod].emplace_back(e->name, it->second.first);
                if (!it->second.second.empty()) tips[mod].emplace_back(e->name, it->second.second);
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
            emit("item_name", names);
            emit("item_tooltip", tips);
            json += "}";
            g_lua_lang_cache[c] = std::move(json);
        }
    }

    static const char *lua_item_language(const language_culture_t culture) {
        const auto it = g_lua_lang_cache.find(static_cast<int>(culture));
        return it == g_lua_lang_cache.end() ? "{}" : it->second.c_str();
    }

    static void lua_item_recipes() {
        for (LuaItemEntry *e: g_lua_items) {
            if (!e->has_recipe) continue;
            // runtime_id 由内核在分配阶段写入，注册时通常还是 -1，故必须读实时值
            const int runtime_id = e->item.runtime_id;
            if (runtime_id < 0) continue;
            std::vector<int> mats;
            mats.reserve(e->recipe_materials.size() * 2);
            for (const auto &m: e->recipe_materials) {
                mats.push_back(m.first);
                mats.push_back(m.second);
            }
            terraria_recipe_set_result(runtime_id, e->recipe_stack);
            if (!mats.empty())
                terraria_recipe_set_material(mats.data(), static_cast<int>(mats.size() / 2));
            terraria_recipe_set_station(e->recipe_station);
            terraria_recipe_add();
        }
    }

    static void ensure_item_hooks() {
        if (g_lua_item_hooks_registered) return;
        g_lua_item_hooks_registered = true;
        terraria_language_manager_register_callback(lua_item_language);
        terraria_recipe_manager_register_callback(lua_item_recipes);
    }

    void shutdown_item_hooks() {
        if (!g_lua_item_hooks_registered) return;
        if (terraria_language_manager_unregister_callback)
            terraria_language_manager_unregister_callback(lua_item_language);
        if (terraria_recipe_manager_unregister_callback)
            terraria_recipe_manager_unregister_callback(lua_item_recipes);
        g_lua_item_hooks_registered = false;
    }

    /// 贴图相对路径解析：依次尝试 lib/Image、lib、<Resources>/Image、<Resources>/Resources/Image、<Resources>。
    std::string resolve_item_image(const std::string &private_dir, const std::string &mod_dir,
                                          const std::string &rel) {
        namespace fs = std::filesystem;
        if (rel.empty()) return {};
        const fs::path r(rel);
        std::error_code ec;
        if (r.is_absolute()) return fs::is_regular_file(r, ec) ? r.string() : std::string();
        for (const auto &part: r)
            if (part == "..") return {};
        std::vector<fs::path> bases;
        if (!mod_dir.empty()) {
            bases.emplace_back(fs::path(mod_dir) / "Image");
            bases.emplace_back(fs::path(mod_dir));
        }
        if (!private_dir.empty()) {
            bases.emplace_back(fs::path(private_dir) / "Image");
            bases.emplace_back(fs::path(private_dir) / "Resources" / "Image");
            bases.emplace_back(fs::path(private_dir));
        }
        for (const auto &b: bases) {
            const fs::path full = b / r;
            if (fs::is_regular_file(full, ec)) return full.string();
        }
        return {};
    }

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
        if (s == "zh" || s == "zh-cn" || s == "zh-hans" || s == "chinese" || s == "chinesesimplified")
            return LANGUAGE_CHINESE_SIMPLIFIED;
        if (s == "pt" || s == "portuguese") return LANGUAGE_PORTUGUESE;
        if (s == "pl" || s == "polish") return LANGUAGE_POLISH;
        if (s == "ja" || s == "japanese") return LANGUAGE_JAPANESE;
        if (s == "ko" || s == "korean") return LANGUAGE_KOREAN;
        if (s == "zh-tw" || s == "zh-hant" || s == "chinesetraditional")
            return LANGUAGE_CHINESE_TRADITIONAL;
        return 0;
    }

    static int l_item_register(lua_State *L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_mod_handle_t *handle = get_handle(L);
        if (!handle) return luaL_error(L, "mod handle missing");

        lua_getfield(L, 1, "name");
        const char *name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        if (!name || !*name) {
            lua_pop(L, 1);
            return luaL_error(L, "mod.item.register: 缺少 name");
        }
        const std::string item_name = name;
        lua_pop(L, 1);

        auto *e = new LuaItemEntry();
        std::memset(&e->item, 0, sizeof(e->item));
        e->owner = handle;
        e->L = L;
        e->name = item_name;
        e->defaults_ref = LUA_NOREF;
        e->can_use_ref = LUA_NOREF;
        e->use_ref = LUA_NOREF;
        e->use_sound = PATCH_NULL;
        e->tex_w = e->tex_h = 0;
        e->tex_tried = false;
        e->has_recipe = false;
        e->recipe_stack = 1;
        e->recipe_station = -1;
        e->registered = false;
        e->runtime_id = -1;

        lua_getfield(L, 1, "tooltip");
        e->item.has_tooltip = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 1, "sacrifice");
        e->item.sacrifice_count = static_cast<int>(luaL_optinteger(L, -1, 0));
        lua_pop(L, 1);

        // animation = { ticks = 8, frames = 9 }：竖向帧图动画（DrawAnimationVertical）
        lua_getfield(L, 1, "animation");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "ticks");
            e->item.anim_ticks = static_cast<int>(luaL_optinteger(L, -1, 8));
            lua_pop(L, 1);
            lua_getfield(L, -1, "frames");
            e->item.anim_frames = static_cast<int>(luaL_optinteger(L, -1, 0));
            lua_pop(L, 1);
            if (e->item.anim_frames < 0) e->item.anim_frames = 0;
        }
        lua_pop(L, 1);

        // use_sound：SoundID 里的字段名，例如 "Item1" / "Item2" / "Item14"
        lua_getfield(L, 1, "use_sound");
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *sname = lua_tostring(L, -1);
            patch_handle_t sound_cls = patchlib_type_get_type("Terraria.ID", "SoundID");
            if (patchlib_is_valid(sound_cls)) {
                patch_handle_t sfield = patchlib_type_get_field(sound_cls, sname);
                if (patchlib_is_valid(sfield)) {
                    patch_handle_t snd = PATCH_NULL;
                    patchlib_field_get_value(sfield, PATCH_NULL, &snd);
                    if (patchlib_is_valid(snd)) e->use_sound = snd;
                    else LOG_WARN("[mod.item] {}: SoundID.{} 是 null", item_name, sname);
                    patchlib_free(sfield);
                } else {
                    LOG_WARN("[mod.item] {}: 找不到 SoundID.{}", item_name, sname);
                }
                patchlib_free(sound_cls);
            }
        }
        lua_pop(L, 1);

        // defaults：表格（声明式）或函数
        lua_getfield(L, 1, "defaults");
        if (lua_isfunction(L, -1)) {
            e->defaults_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            if (lua_istable(L, -1)) {
                const patch_handle_t item_class = patchlib_type_get_type("Terraria", "Item");
                if (patchlib_is_valid(item_class)) {
                    lua_pushnil(L);
                    while (lua_next(L, -2)) {
                        if (lua_type(L, -2) == LUA_TSTRING) {
                            const char *fname = lua_tostring(L, -2);
                            const patch_handle_t f = patchlib_type_get_field(item_class, fname);
                            if (patchlib_is_valid(f)) {
                                const patch_type_t t = patchlib_field_get_type(f);
                                uint64_t v = 0;
                                if (try_read_patch_value(L, -1, t, &v)) {
                                    e->defaults.push_back({f, t, v, ref_default_value(L, -1, t)});
                                } else {
                                    LOG_WARN("[mod.item] {}: 字段 {} 值类型不匹配，已忽略",
                                             item_name, fname);
                                }
                            } else {
                                LOG_WARN("[mod.item] {}: 找不到 Item 字段 {}", item_name, fname);
                            }
                        }
                        lua_pop(L, 1);
                    }
                }
            }
            lua_pop(L, 1);
        }

        // prefixes：声明可重铸的词缀池（不写 = 不可重铸）
        //   sword/melee → 1, spear → 2, gun/bow/ranged → 3, magic → 4,
        //   summon → 5, boomerang/thrown → 6, accessory → 7
        lua_getfield(L, 1, "prefixes");
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *pv = lua_tostring(L, -1);
            if (!pv) pv = "";
            if (!strcmp(pv, "sword") || !strcmp(pv, "melee")) e->prefix_class = 1;
            else if (!strcmp(pv, "spear")) e->prefix_class = 2;
            else if (!strcmp(pv, "gun") || !strcmp(pv, "bow") || !strcmp(pv, "ranged")) e->prefix_class = 3;
            else if (!strcmp(pv, "magic")) e->prefix_class = 4;
            else if (!strcmp(pv, "summon")) e->prefix_class = 5;
            else if (!strcmp(pv, "boomerang") || !strcmp(pv, "thrown")) e->prefix_class = 6;
            else if (!strcmp(pv, "accessory")) e->prefix_class = 7;
            else LOG_WARN("[mod.item] {}: 未知 prefixes 值 {}（忽略）", item_name, pv);
        } else if (lua_type(L, -1) == LUA_TNUMBER) {
            e->prefix_class = (int) lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "can_use");
        if (lua_isfunction(L, -1)) e->can_use_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        lua_getfield(L, 1, "use");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "on_use"); }
        if (lua_isfunction(L, -1)) e->use_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else lua_pop(L, 1);

        // language：{ en = {name=, tooltip=}, zh = {...} }
        lua_getfield(L, 1, "language");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                int culture = 0;
                if (lua_type(L, -2) == LUA_TSTRING) culture = culture_from_key(lua_tostring(L, -2));
                else if (lua_isnumber(L, -2)) culture = static_cast<int>(lua_tointeger(L, -2));
                if (culture > 0 && lua_istable(L, -1)) {
                    std::string dn, dt;
                    lua_getfield(L, -1, "name");
                    if (lua_type(L, -1) == LUA_TSTRING) dn = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "tooltip");
                    if (lua_type(L, -1) == LUA_TSTRING) dt = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    e->lang[culture] = std::make_pair(dn, dt);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        // recipe：{ result=1, materials={{id=9,count=10}}, station=18 }
        lua_getfield(L, 1, "recipe");
        if (lua_istable(L, -1)) {
            e->has_recipe = true;
            lua_getfield(L, -1, "result");
            e->recipe_stack = static_cast<int>(luaL_optinteger(L, -1, 1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "station");
            e->recipe_station = static_cast<int>(luaL_optinteger(L, -1, -1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "materials");
            if (lua_istable(L, -1)) {
                const int n = static_cast<int>(lua_rawlen(L, -1));
                for (int i = 1; i <= n; ++i) {
                    lua_rawgeti(L, -1, i);
                    if (lua_istable(L, -1)) {
                        int id = 0, cnt = 1;
                        lua_getfield(L, -1, "id");
                        if (lua_isnumber(L, -1)) {
                            id = static_cast<int>(lua_tointeger(L, -1));
                        } else {
                            lua_pop(L, 1);
                            lua_rawgeti(L, -1, 1);
                            if (lua_isnumber(L, -1)) id = static_cast<int>(lua_tointeger(L, -1));
                        }
                        lua_pop(L, 1);
                        lua_getfield(L, -1, "count");
                        if (lua_isnumber(L, -1)) {
                            cnt = static_cast<int>(lua_tointeger(L, -1));
                        } else {
                            lua_pop(L, 1);
                            lua_rawgeti(L, -1, 2);
                            if (lua_isnumber(L, -1)) cnt = static_cast<int>(lua_tointeger(L, -1));
                        }
                        lua_pop(L, 1);
                        if (id > 0 && cnt > 0) e->recipe_materials.emplace_back(id, cnt);
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        // texture
        lua_getfield(L, 1, "texture");
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *rel = lua_tostring(L, -1);
            e->texture_path = resolve_item_image(handle->private_dir, handle->mod_dir, rel);
            if (e->texture_path.empty())
                LOG_WARN("[mod.item] {}: 未找到贴图 {}", item_name, rel);
        }
        lua_pop(L, 1);

        // 组装内核句柄
        e->item.parent_modloader_id = LUALOADER_PKG_ID;
        e->item.parent_id = handle->mod_id.c_str();
        e->item.internal_name = e->name.c_str();
        e->item.runtime_id = -1;
        e->item.item_ops.early_init = nullptr;
        e->item.item_ops.init_static = nullptr;
        e->item.item_ops.set_defaults = lua_item_set_defaults;
        e->item.item_ops.can_use = (e->can_use_ref != LUA_NOREF) ? lua_item_can_use : nullptr;
        e->item.item_ops.use = (e->use_ref != LUA_NOREF) ? lua_item_use : nullptr;
        e->item.item_ops.get_texture = lua_item_get_texture;

        if (!terraria_item_manager_register_item(&e->item)) {
            LOG_ERROR("[mod.item] {}: 注册失败", item_name);
            if (e->defaults_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->defaults_ref);
            if (e->can_use_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->can_use_ref);
            if (e->use_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->use_ref);
            unref_default_values(L, e->defaults);
            if (patchlib_is_valid(e->use_sound)) patchlib_free(e->use_sound);
            delete e;
            lua_pushnil(L);
            return 1;
        }

        // 声明可重铸类别（不声明 = 不可重铸）
        if (e->prefix_class > 0)
            terraria_item_manager_set_prefix_class(&e->item, e->prefix_class);

        e->registered = true;
        e->runtime_id = e->item.runtime_id;
        g_lua_items.push_back(e);
        ensure_item_hooks();
        rebuild_language_cache();
        LOG_INFO("[mod.item] 已注册 {} (runtime_id={}, use={}, can_use={})", item_name, e->runtime_id,
                 e->use_ref != LUA_NOREF, e->can_use_ref != LUA_NOREF);
        lua_pushinteger(L, e->runtime_id);
        return 1;
    }

    static int l_item_runtime_id(lua_State *L) {
        const char *name = luaL_checkstring(L, 1);
        lua_mod_handle_t *handle = get_handle(L);
        for (LuaItemEntry *e: g_lua_items) {
            if (e->owner == handle && e->name == name) {
                if (e->item.runtime_id >= 0) lua_pushinteger(L, e->item.runtime_id);
                else lua_pushnil(L);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    static const luaL_Reg mod_item_functions[] = {
            {"register", l_item_register},
            {"runtime_id", l_item_runtime_id},
            {nullptr, nullptr},
    };

    void unregister_items(lua_mod_handle_t *handle) {
        if (!handle) return;
        for (size_t i = g_lua_items.size(); i-- > 0;) {
            LuaItemEntry *e = g_lua_items[i];
            if (e->owner != handle) continue;
            if (e->registered)
                terraria_item_manager_unregister_item(&e->item);
            if (handle->L) {
                if (e->defaults_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->defaults_ref);
                if (e->can_use_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->can_use_ref);
                if (e->use_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->use_ref);
                unref_default_values(handle->L, e->defaults);
            }
            if (patchlib_is_valid(e->use_sound)) patchlib_free(e->use_sound);
            delete e;
            g_lua_items.erase(g_lua_items.begin() + static_cast<long>(i));
        }
        rebuild_language_cache();
    }


void add_item(lua_State *L) {
    luaL_setfuncs(L, mod_item_functions, 0);
}

} // namespace lualoader::lua_api
