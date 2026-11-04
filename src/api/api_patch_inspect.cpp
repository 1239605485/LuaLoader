#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ========================================================================
    // 类型/成员内省（对应内核 type_*/method_*/field_* 系列）
    // ========================================================================

    /// mod.patch.get_full_name(type) -> "命名空间.类名" | nil
    static int l_patch_get_full_name(lua_State *L) {
        char *full = patchlib_type_get_full_name(to_handle(L, 1));
        if (!full) { lua_pushnil(L); return 1; }
        lua_pushstring(L, full); // 内核用 malloc 分配，拷贝后释放
        free(full);
        return 1;
    }

    /// mod.patch.get_namespace(type) -> string | nil
    static int l_patch_get_namespace(lua_State *L) {
        const char *ns = patchlib_type_get_namespace(to_handle(L, 1));
        if (ns) lua_pushstring(L, ns); else lua_pushnil(L);
        return 1;
    }

    // 把内核填充的句柄向量逐个压成 Lua 表（元素为 patch_handle_t）
    static void push_handle_table_from_vector(lua_State *L, tefstd_vector_t *vec) {
        const size_t n = tefstd_vector_size(vec);
        lua_createtable(L, static_cast<int>(n), 0);
        for (size_t i = 0; i < n; ++i) {
            auto *slot = static_cast<patch_handle_t *>(tefstd_vector_at(vec, i));
            push_returned_object(L, slot ? *slot : nullptr);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    }

    // 通用：type_get_xxx(type, including_parent, vector)
    template<typename Fn>
    static int type_member_list(lua_State *L, Fn fn) {
        const patch_handle_t type = to_handle(L, 1);
        const bool including_parent = lua_isnoneornil(L, 2) ? false : (lua_toboolean(L, 2) != 0);
        if (!type) { lua_pushnil(L); return 1; }

        tefstd_vector_t vec;
        if (!tefstd_vector_init(&vec, sizeof(patch_handle_t))) {
            lua_pushnil(L);
            return 1;
        }
        const bool ok = fn(type, including_parent, &vec);
        if (!ok) {
            tefstd_vector_destroy(&vec);
            lua_pushnil(L);
            return 1;
        }
        push_handle_table_from_vector(L, &vec);
        tefstd_vector_destroy(&vec);
        return 1;
    }

    /// mod.patch.get_fields(type[, including_parent]) -> { field, ... } | nil
    static int l_patch_get_fields(lua_State *L) {
        return type_member_list(L, [](patch_handle_t t, bool p, tefstd_vector_t *v) {
            return patchlib_type_get_fields(t, p, v);
        });
    }

    /// mod.patch.get_methods(type[, including_parent]) -> { method, ... } | nil
    static int l_patch_get_methods(lua_State *L) {
        return type_member_list(L, [](patch_handle_t t, bool p, tefstd_vector_t *v) {
            return patchlib_type_get_methods(t, p, v);
        });
    }

    /// mod.patch.get_properties(type[, including_parent]) -> { property, ... } | nil
    static int l_patch_get_properties(lua_State *L) {
        return type_member_list(L, [](patch_handle_t t, bool p, tefstd_vector_t *v) {
            return patchlib_type_get_properties(t, p, v);
        });
    }

    /// mod.patch.get_inner_types(type[, including_parent]) -> { type, ... } | nil
    static int l_patch_get_inner_types(lua_State *L) {
        return type_member_list(L, [](patch_handle_t t, bool p, tefstd_vector_t *v) {
            return patchlib_type_get_inner_types(t, p, v);
        });
    }

    // 从 Lua 表读取类型句柄数组（用于按类型选重载/泛型）
    static int read_type_array(lua_State *L, int idx, patch_handle_t *out, int max_n) {
        luaL_checktype(L, idx, LUA_TTABLE);
        const int n = static_cast<int>(luaL_len(L, idx));
        if (n < 0 || n > max_n) return -1;
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            out[i - 1] = to_handle(L, -1);
            lua_pop(L, 1);
        }
        return n;
    }

    /// mod.patch.get_method_by_param_types(type, name, { type, ... }) -> method | nil
    static int l_patch_get_method_by_param_types(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const char *name = luaL_checkstring(L, 2);
        patch_handle_t types[MAX_CALL_ARGS] = {nullptr};
        const int n = read_type_array(L, 3, types, MAX_CALL_ARGS);
        if (n < 0) return luaL_error(L, "bad type array");
        push_returned_object(L, type ? patchlib_type_get_method_by_param_types(type, name, n, types) : nullptr);
        return 1;
    }

    /// mod.patch.get_method_by_signature(type, name, { type, ... }, { "name", ... }) -> method | nil
    static int l_patch_get_method_by_signature(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const char *name = luaL_checkstring(L, 2);
        patch_handle_t types[MAX_CALL_ARGS] = {nullptr};
        const int n = read_type_array(L, 3, types, MAX_CALL_ARGS);
        if (n < 0) return luaL_error(L, "bad type array");

        std::vector<std::string> store;
        std::vector<const char *> names;
        if (!lua_isnoneornil(L, 4)) {
            luaL_checktype(L, 4, LUA_TTABLE);
            const int m = static_cast<int>(luaL_len(L, 4));
            if (m != n) return luaL_error(L, "names count mismatch");
            store.reserve(m);
            for (int i = 1; i <= m; ++i) {
                lua_rawgeti(L, 4, i);
                store.emplace_back(luaL_checkstring(L, -1));
                lua_pop(L, 1);
            }
            for (auto &s: store) names.push_back(s.c_str());
        }
        push_returned_object(L, type ? patchlib_type_get_method_by_signature(
                type, name, n, types, names.empty() ? nullptr : names.data()) : nullptr);
        return 1;
    }

    // 用 Lua 表里的类型句柄填充 tefstd_vector_t（泛型参数）
    static bool fill_type_vector(lua_State *L, int idx, tefstd_vector_t *vec) {
        patch_handle_t types[MAX_CALL_ARGS] = {nullptr};
        const int n = read_type_array(L, idx, types, MAX_CALL_ARGS);
        if (n < 0) return false;
        if (!tefstd_vector_init(vec, sizeof(patch_handle_t))) return false;
        for (int i = 0; i < n; ++i) {
            // Android 上 MakeGenericType/MakeGenericMethod 需要 System.Type(MonoType)，
            // 而 Lua 侧 get_type 拿到的是 Il2CppClass*，这里统一转换。
            // （桌面端 patchlib_type_get_mono_type 是恒等宏）
            patch_handle_t mt = patchlib_type_get_mono_type(types[i]);
            if (!mt) {
                tefstd_vector_destroy(vec);
                return false;
            }
            if (!tefstd_vector_push_back(vec, &mt)) {
                tefstd_vector_destroy(vec);
                return false;
            }
        }
        return true;
    }

    /// mod.patch.make_generic_type(generic_type_def, { type, ... }) -> type | nil
    static int l_patch_make_generic_type(lua_State *L) {
        const patch_handle_t def = to_handle(L, 1);
        tefstd_vector_t vec;
        if (!fill_type_vector(L, 2, &vec)) { lua_pushnil(L); return 1; }
        push_returned_object(L, def ? patchlib_type_make_generic_type(def, &vec) : nullptr);
        tefstd_vector_destroy(&vec);
        return 1;
    }

    /// mod.patch.make_generic_instance(method, { type, ... }) -> method | nil
    static int l_patch_make_generic_instance(lua_State *L) {
        const patch_handle_t m = to_handle(L, 1);
        tefstd_vector_t vec;
        if (!fill_type_vector(L, 2, &vec)) { lua_pushnil(L); return 1; }
        push_returned_object(L, m ? patchlib_method_make_generic_instance(m, &vec) : nullptr);
        tefstd_vector_destroy(&vec);
        return 1;
    }

    /// mod.patch.method_name(method) -> string | nil
    static int l_patch_method_name(lua_State *L) {
        const patch_handle_t m = to_handle(L, 1);
        const char *n = m ? patchlib_method_get_name(m) : nullptr;
        if (n) lua_pushstring(L, n); else lua_pushnil(L);
        return 1;
    }

    /// mod.patch.method_param_count(method) -> int
    static int l_patch_method_param_count(lua_State *L) {
        const patch_handle_t m = to_handle(L, 1);
        lua_pushinteger(L, m ? static_cast<lua_Integer>(patchlib_method_get_param_count(m)) : 0);
        return 1;
    }

    /// mod.patch.method_token(method) -> int（可当缓存键）
    static int l_patch_method_token(lua_State *L) {
        const patch_handle_t m = to_handle(L, 1);
        lua_pushinteger(L, m ? static_cast<lua_Integer>(patchlib_method_get_token(m)) : 0);
        return 1;
    }

    /// mod.patch.method_is_instance(method) -> bool
    static int l_patch_method_is_instance(lua_State *L) {
        const patch_handle_t m = to_handle(L, 1);
        lua_pushboolean(L, m && patchlib_method_is_instance(m));
        return 1;
    }

    /// mod.patch.method_is_static(method) -> bool
    static int l_patch_method_is_static(lua_State *L) {
        const patch_handle_t m = to_handle(L, 1);
        lua_pushboolean(L, m && patchlib_method_is_static(m));
        return 1;
    }

    /// mod.patch.field_name(field) -> string | nil
    static int l_patch_field_name(lua_State *L) {
        const patch_handle_t f = to_handle(L, 1);
        const char *n = f ? patchlib_field_get_name(f) : nullptr;
        if (n) lua_pushstring(L, n); else lua_pushnil(L);
        return 1;
    }

    /// mod.patch.field_is_const(field) -> bool
    static int l_patch_field_is_const(lua_State *L) {
        const patch_handle_t f = to_handle(L, 1);
        lua_pushboolean(L, f && patchlib_field_is_const(f));
        return 1;
    }

    /// mod.patch.field_is_instance(field) -> bool
    static int l_patch_field_is_instance(lua_State *L) {
        const patch_handle_t f = to_handle(L, 1);
        lua_pushboolean(L, f && patchlib_field_is_instance(f));
        return 1;
    }

    /// mod.patch.field_is_static(field) -> bool
    static int l_patch_field_is_static(lua_State *L) {
        const patch_handle_t f = to_handle(L, 1);
        lua_pushboolean(L, f && patchlib_field_is_static(f));
        return 1;
    }

    /// mod.patch.property_name(property) -> string | nil
    static int l_patch_property_name(lua_State *L) {
        const patch_handle_t p = to_handle(L, 1);
        const char *n = p ? patchlib_property_get_name(p) : nullptr;
        if (n) lua_pushstring(L, n); else lua_pushnil(L);
        return 1;
    }

    /// mod.patch.array_empty(array) -> bool
    static int l_patch_array_empty(lua_State *L) {
        const patch_handle_t a = to_handle(L, 1);
        lua_pushboolean(L, a && patchlib_array_empty(a));
        return 1;
    }


void add_patch_inspect(lua_State *L) {
    static const luaL_Reg fns[] = {
            {"get_full_name", l_patch_get_full_name},
            {"get_namespace", l_patch_get_namespace},
            {"get_fields", l_patch_get_fields},
            {"get_methods", l_patch_get_methods},
            {"get_properties", l_patch_get_properties},
            {"get_inner_types", l_patch_get_inner_types},
            {"get_method_by_param_types", l_patch_get_method_by_param_types},
            {"get_method_by_signature", l_patch_get_method_by_signature},
            {"make_generic_type", l_patch_make_generic_type},
            {"make_generic_instance", l_patch_make_generic_instance},
            {"method_name", l_patch_method_name},
            {"method_param_count", l_patch_method_param_count},
            {"method_token", l_patch_method_token},
            {"method_is_instance", l_patch_method_is_instance},
            {"method_is_static", l_patch_method_is_static},
            {"field_name", l_patch_field_name},
            {"field_is_const", l_patch_field_is_const},
            {"field_is_instance", l_patch_field_is_instance},
            {"field_is_static", l_patch_field_is_static},
            {"property_name", l_patch_property_name},
            {"array_empty", l_patch_array_empty},
            {nullptr, nullptr},
    };
    luaL_setfuncs(L, fns, 0);
}

} // namespace lualoader::lua_api
