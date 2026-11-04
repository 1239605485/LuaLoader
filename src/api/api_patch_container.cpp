#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ========================================================================
    // 容器（Dictionary / List）——值按显式类型字符串编组
    // ========================================================================

    // 把 Lua 第 idx 个参数按类型字符串读进 8 字节存储；返回存储指针
    static void *read_typed_value(lua_State *L, int idx, const char *type_name, uint64_t *storage) {
        patch_type_t type;
        if (!type_name || !type_from_name(type_name, &type))
            return nullptr;
        read_patch_value(L, idx, type, storage);
        return storage;
    }

    /// mod.patch.dictionary_create(key_type, value_type[, capacity]) -> dict
    static int l_patch_dictionary_create(lua_State *L) {
        const patch_handle_t key_type = to_handle(L, 1);
        const patch_handle_t value_type = to_handle(L, 2);
        const size_t capacity = lua_isnoneornil(L, 3)
                                    ? 0 : static_cast<size_t>(luaL_checkinteger(L, 3));
        push_owned_handle(L, (key_type && value_type)
                                 ? patchlib_dictionary_create(key_type, value_type, capacity)
                                 : nullptr);
        return 1;
    }

    /// mod.patch.dictionary_add(dict, key, key_type, value, value_type) -> bool
    static int l_patch_dictionary_add(lua_State *L) {
        const patch_handle_t dict = to_handle(L, 1);
        const char *kt = luaL_checkstring(L, 3);
        const char *vt = luaL_checkstring(L, 5);
        uint64_t ks = 0, vs = 0;
        void *k = read_typed_value(L, 2, kt, &ks);
        void *v = read_typed_value(L, 4, vt, &vs);
        if (!dict || !k || !v) return luaL_error(L, "bad dictionary/key/value");
        lua_pushboolean(L, patchlib_dictionary_add(dict, k, v));
        return 1;
    }

    /// mod.patch.dictionary_set_value(dict, key, key_type, value, value_type) -> bool
    static int l_patch_dictionary_set_value(lua_State *L) {
        const patch_handle_t dict = to_handle(L, 1);
        const char *kt = luaL_checkstring(L, 3);
        const char *vt = luaL_checkstring(L, 5);
        uint64_t ks = 0, vs = 0;
        void *k = read_typed_value(L, 2, kt, &ks);
        void *v = read_typed_value(L, 4, vt, &vs);
        if (!dict || !k || !v) return luaL_error(L, "bad dictionary/key/value");
        lua_pushboolean(L, patchlib_dictionary_set_value(dict, k, v));
        return 1;
    }

    /// mod.patch.dictionary_get_value(dict, key, key_type, value_type) -> value | nil
    static int l_patch_dictionary_get_value(lua_State *L) {
        const patch_handle_t dict = to_handle(L, 1);
        const char *kt = luaL_checkstring(L, 3);
        const char *vt = luaL_checkstring(L, 4);
        uint64_t ks = 0, out = 0;
        void *k = read_typed_value(L, 2, kt, &ks);
        patch_type_t vtype;
        if (!dict || !k || !vt || !type_from_name(vt, &vtype))
            return luaL_error(L, "bad dictionary/key/value_type");
        if (!patchlib_dictionary_get_value(dict, k, &out)) {
            lua_pushnil(L);
            return 1;
        }
        if (vtype == PATCH_OBJECT)
            push_returned_object(L, *reinterpret_cast<patch_handle_t *>(&out));
        else
            push_patch_value(L, vtype, &out);
        return 1;
    }

    /// mod.patch.dictionary_length(dict) -> int
    static int l_patch_dictionary_length(lua_State *L) {
        const patch_handle_t dict = to_handle(L, 1);
        lua_pushinteger(L, dict ? static_cast<lua_Integer>(patchlib_dictionary_length(dict)) : 0);
        return 1;
    }

    /// mod.patch.dictionary_remove(dict, key, key_type) -> bool
    static int l_patch_dictionary_remove(lua_State *L) {
        const patch_handle_t dict = to_handle(L, 1);
        const char *kt = luaL_checkstring(L, 3);
        uint64_t ks = 0;
        void *k = read_typed_value(L, 2, kt, &ks);
        if (!dict || !k) return luaL_error(L, "bad dictionary/key");
        lua_pushboolean(L, patchlib_dictionary_remove(dict, k));
        return 1;
    }

    /// mod.patch.dictionary_clear(dict) -> bool
    static int l_patch_dictionary_clear(lua_State *L) {
        const patch_handle_t dict = to_handle(L, 1);
        lua_pushboolean(L, dict && patchlib_dictionary_clear(dict));
        return 1;
    }

    /// mod.patch.list_create(type[, capacity]) -> list
    static int l_patch_list_create(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const size_t capacity = lua_isnoneornil(L, 2)
                                    ? 0 : static_cast<size_t>(luaL_checkinteger(L, 2));
        push_owned_handle(L, type ? patchlib_list_create(capacity, type) : nullptr);
        return 1;
    }

    /// mod.patch.list_add(list, value, value_type) -> bool
    static int l_patch_list_add(lua_State *L) {
        const patch_handle_t list = to_handle(L, 1);
        const char *vt = luaL_checkstring(L, 3);
        uint64_t vs = 0;
        void *v = read_typed_value(L, 2, vt, &vs);
        if (!list || !v) return luaL_error(L, "bad list/value");
        lua_pushboolean(L, patchlib_list_add(list, v));
        return 1;
    }

    /// mod.patch.list_remove(list, value, value_type) -> bool
    static int l_patch_list_remove(lua_State *L) {
        const patch_handle_t list = to_handle(L, 1);
        const char *vt = luaL_checkstring(L, 3);
        uint64_t vs = 0;
        void *v = read_typed_value(L, 2, vt, &vs);
        if (!list || !v) return luaL_error(L, "bad list/value");
        lua_pushboolean(L, patchlib_list_remove(list, v));
        return 1;
    }

    /// mod.patch.list_remove_at(list, index) -> bool（0 基）
    static int l_patch_list_remove_at(lua_State *L) {
        const patch_handle_t list = to_handle(L, 1);
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        lua_pushboolean(L, list && patchlib_list_remove_at(list, index));
        return 1;
    }

    /// mod.patch.list_clear(list) -> bool
    static int l_patch_list_clear(lua_State *L) {
        const patch_handle_t list = to_handle(L, 1);
        lua_pushboolean(L, list && patchlib_list_clear(list));
        return 1;
    }

    /// mod.patch.list_copy_from(list, array) -> bool
    static int l_patch_list_copy_from(lua_State *L) {
        const patch_handle_t list = to_handle(L, 1);
        const patch_handle_t array = to_handle(L, 2);
        lua_pushboolean(L, list && array && patchlib_list_copy_from(list, array));
        return 1;
    }

    /// mod.patch.list_get_array(list) -> array | nil
    static int l_patch_list_get_array(lua_State *L) {
        const patch_handle_t list = to_handle(L, 1);
        push_returned_object(L, list ? patchlib_list_get_array(list) : nullptr);
        return 1;
    }


void add_patch_container(lua_State *L) {
    static const luaL_Reg fns[] = {
            {"dictionary_create", l_patch_dictionary_create},
            {"dictionary_add", l_patch_dictionary_add},
            {"dictionary_set_value", l_patch_dictionary_set_value},
            {"dictionary_get_value", l_patch_dictionary_get_value},
            {"dictionary_length", l_patch_dictionary_length},
            {"dictionary_remove", l_patch_dictionary_remove},
            {"dictionary_clear", l_patch_dictionary_clear},
            {"list_create", l_patch_list_create},
            {"list_add", l_patch_list_add},
            {"list_remove", l_patch_list_remove},
            {"list_remove_at", l_patch_list_remove_at},
            {"list_clear", l_patch_list_clear},
            {"list_copy_from", l_patch_list_copy_from},
            {"list_get_array", l_patch_list_get_array},
            {nullptr, nullptr},
    };
    luaL_setfuncs(L, fns, 0);
}

} // namespace lualoader::lua_api
