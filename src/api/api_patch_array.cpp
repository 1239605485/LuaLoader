#include "api_internal.hpp"

#include <cstdint>

namespace lualoader::lua_api {

    // ========================================================================
    // Patchlib: 结构体字段(Vector2 等) / 数组
    // ========================================================================

    // 注意: 不要调用 patchlib_field_get_size —— 内核的符号注入表里没有它，
    // 在模块中会是 NULL 函数指针, 调用即崩溃。尺寸用 patchlib_field_get_type +
    // get_size_from_patch_type 推断, 或由调用方显式给出。

    /// 读取字段的原始字节。Android/IL2CPP 走字段真实指针，桌面端走 get_value。
    static bool read_field_bytes(const patch_handle_t field, const patch_handle_t instance, void *out,
                                 const size_t size) {
        if (!field || !out || size == 0 || size > 16) return false;
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (!ptr) return false;
        std::memcpy(out, ptr, size);
        return true;
#else
        std::memset(out, 0, size);
        patchlib_field_get_value(field, instance, out);
        return true;
#endif
    }

    /// 写入字段的原始字节。Android/IL2CPP 走字段真实指针，桌面端走 set_value。
    static bool write_field_bytes(const patch_handle_t field, const patch_handle_t instance, const void *in,
                                  const size_t size) {
        if (!field || !in || size == 0 || size > 16) return false;
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (!ptr) return false;
        std::memcpy(ptr, in, size);
        return true;
#else
        patchlib_field_set_value(field, instance, const_cast<void *>(in));
        return true;
#endif
    }

    /// 推断字段大小(1~16)，失败返回 0。不使用未注入的 patchlib_field_get_size。
    static size_t guess_field_size(const patch_handle_t field) {
        if (!field) return 0;
        const size_t size = get_size_from_patch_type(patchlib_field_get_type(field));
        return size;
    }

    /// 读取 Vector2(或任意以两个 float 开头的 8 字节结构体)字段 → x, y
    static int l_patch_field_get_vec2(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (instance_missing(L, 2, field)) {
            lua_pushnil(L);
            lua_pushnil(L);
            return 2;
        }
        float v[2] = {0.0f, 0.0f};
        if (!read_field_bytes(field, instance, v, sizeof(v))) {
            lua_pushnil(L);
            lua_pushnil(L);
            return 2;
        }
        lua_pushnumber(L, v[0]);
        lua_pushnumber(L, v[1]);
        return 2;
    }

    /// 写入 Vector2(或任意以两个 float 开头的 8 字节结构体)字段 ← x, y
    static int l_patch_field_set_vec2(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (instance_missing(L, 2, field)) return 0;
        const float v[2] = {
                static_cast<float>(luaL_checknumber(L, 3)),
                static_cast<float>(luaL_checknumber(L, 4)),
        };
        if (!write_field_bytes(field, instance, v, sizeof(v))) {
            return luaL_error(L, "cannot write vector field");
        }
        return 0;
    }

    /// 读取字段原始字节，返回 Lua 字符串。
    /// 第三个参数为可选字节数；省略时按字段类型推断(失败则取 8)。
    static int l_patch_field_get_raw(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (instance_missing(L, 2, field)) {
            lua_pushnil(L);
            return 1;
        }

        size_t size;
        if (lua_isnoneornil(L, 3)) {
            size = guess_field_size(field);
            if (size == 0) size = 8; // 结构体等未知类型默认 8 字节
        } else {
            const lua_Integer requested = luaL_checkinteger(L, 3);
            size = requested > 0 ? static_cast<size_t>(requested) : 0;
        }
        if (size == 0 || size > 16) {
            lua_pushnil(L);
            return 1;
        }
        unsigned char buffer[16] = {};
        if (!read_field_bytes(field, instance, buffer, size)) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, reinterpret_cast<const char *>(buffer), size);
        return 1;
    }

    /// 以 Lua 字符串写入字段原始字节（长度 1~16）
    static int l_patch_field_set_raw(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (instance_missing(L, 2, field)) return 0;
        size_t len = 0;
        const char *data = luaL_checklstring(L, 3, &len);
        if (len == 0 || len > 16) {
            return luaL_error(L, "raw data length out of range (1..16)");
        }
        if (!write_field_bytes(field, instance, data, len)) {
            return luaL_error(L, "cannot write raw field");
        }
        return 0;
    }

    /// 数组长度
    static int l_patch_array_length(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        lua_pushinteger(L, static_cast<lua_Integer>(patchlib_array_length(array)));
        return 1;
    }

    /// 数组元素真实大小（未知按指针大小；上限 1MB 防止异常数组）
    static size_t array_element_bytes(const patch_handle_t array) {
        size_t n = patchlib_array_element_size(array);
        if (n == 0) n = sizeof(void *);
        if (n > (1u << 20)) n = (1u << 20);
        return n;
    }

    static bool array_is_reference(const patch_handle_t array, const size_t elem_bytes) {
        return patchlib_array_element_is_reference
                   ? patchlib_array_element_is_reference(array)
                   : (elem_bytes == sizeof(void *));
    }

    static bool is_array_type_compatible(const size_t raw_elem_bytes, const bool is_ref, const patch_type_t type) {
        if (type == PATCH_OBJECT) {
            return is_ref && (raw_elem_bytes == 0 || raw_elem_bytes == sizeof(void *));
        }
        if (is_ref) return false;
        if (type == PATCH_POINTER) {
            return raw_elem_bytes == 0 || raw_elem_bytes == sizeof(void *);
        }
        if (type == PATCH_CHAR) {
            return raw_elem_bytes == 0 || raw_elem_bytes == 1 || raw_elem_bytes == 2;
        }
        const size_t expected = patch_scalar_size(type);
        if (expected == 0) return false;
        return raw_elem_bytes == 0 || raw_elem_bytes == expected;
    }

    /// 数组取元素。第三个参数为元素类型名，省略时按对象(指针)处理。
    static int l_patch_array_at(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        const size_t raw_elem_bytes = patchlib_array_element_size(array);
        const size_t elem_bytes = array_element_bytes(array);
        const bool is_ref = array_is_reference(array, elem_bytes);

        patch_type_t type = PATCH_OBJECT;
        bool want_string = false;
        if (!lua_isnoneornil(L, 3)) {
            const char *type_name = luaL_checkstring(L, 3);
            if (std::strcmp(type_name, "string") == 0) {
                want_string = true;
                type = PATCH_OBJECT;
            } else if (!type_from_name(type_name, &type)) {
                return luaL_error(L, "unknown type name: %s", type_name);
            }
            if (!is_array_type_compatible(raw_elem_bytes, is_ref, type)) {
                return luaL_error(L, "array element type mismatch for '%s'", type_name);
            }
        }

        /* 关键：内核按元素真实大小 memcpy，同时标量读取至少需要 8 字节安全缓冲 */
        std::vector<unsigned char> storage(elem_bytes >= 8 ? elem_bytes : 8, 0);
        void *buffer = storage.data();
        if (!patchlib_array_at(array, index, buffer)) {
            lua_pushnil(L);
            return 1;
        }
        if (lua_isnoneornil(L, 3)) {
            /* 省略类型只对「引用类型元素」的数组返回对象句柄。 */
            if (!is_ref || (raw_elem_bytes != 0 && raw_elem_bytes != sizeof(void *))) {
                lua_pushnil(L);
                return 1;
            }
            void *ptr = *static_cast<void **>(buffer);
            if (ptr) lua_pushlightuserdata(L, ptr);
            else lua_pushnil(L);
            return 1;
        }
        if (want_string) {
            push_managed_string(L, *static_cast<void **>(buffer));
            return 1;
        }
        push_patch_value(L, type, buffer);
        return 1;
    }

    // ========================================================================
    // Patchlib: 数组写入 / 创建 / 原始字节 / 属性桥接 / 按名选重载
    // ========================================================================

    /// 依据 Lua 值推断写入类型（显式类型由调用方给出）
    static patch_type_t infer_set_type(lua_State *L, const int idx, void *out, std::vector<patch_handle_t> &owned) {
        const int t = lua_type(L, idx);
        if (t == LUA_TBOOLEAN) {
            *static_cast<bool *>(out) = lua_toboolean(L, idx) != 0;
            return PATCH_BOOL;
        }
        if (t == LUA_TNUMBER) {
            if (lua_isinteger(L, idx)) {
                *static_cast<int32_t *>(out) = static_cast<int32_t>(lua_tointeger(L, idx));
                return PATCH_INT32;
            }
            *static_cast<float *>(out) = static_cast<float>(lua_tonumber(L, idx));
            return PATCH_FLOAT;
        }
        if (t == LUA_TLIGHTUSERDATA || luaL_testudata(L, idx, OWNED_HANDLE_MT)) {
            *static_cast<void **>(out) = to_handle(L, idx);
            return PATCH_OBJECT;
        }
        if (t == LUA_TSTRING) {
            patch_handle_t s = patchlib_string_create(lua_tostring(L, idx));
            owned.push_back(s);
            *static_cast<void **>(out) = s;
            return PATCH_OBJECT;
        }
        if (t == LUA_TNIL || t == LUA_TNONE) {
            *static_cast<void **>(out) = nullptr;
            return PATCH_OBJECT;
        }
        luaL_error(L, "unsupported array element value type: %s", luaL_typename(L, idx));
        return PATCH_VOID;
    }

    /// 计算单个元素的写入缓冲；type_idx 为显式类型参数位置（无则按值推断）
    static patch_type_t read_element(lua_State *L, const int val_idx, const int type_idx, void *out,
                                     std::vector<patch_handle_t> &owned) {
        if (!lua_isnoneornil(L, type_idx)) {
            const char *tn = luaL_checkstring(L, type_idx);
            if (std::strcmp(tn, "string") == 0 || std::strcmp(tn, "object") == 0) {
                if (lua_type(L, val_idx) == LUA_TSTRING) {
                    patch_handle_t s = patchlib_string_create(lua_tostring(L, val_idx));
                    owned.push_back(s);
                    *static_cast<void **>(out) = s;
                } else {
                    if (!lua_isnoneornil(L, val_idx) &&
                        lua_type(L, val_idx) != LUA_TLIGHTUSERDATA &&
                        !luaL_testudata(L, val_idx, OWNED_HANDLE_MT)) {
                        luaL_error(L, "handle, string, or nil expected for '%s'", tn);
                    }
                    *static_cast<void **>(out) = to_handle(L, val_idx);
                }
                return PATCH_OBJECT;
            }
            patch_type_t type;
            if (!type_from_name(tn, &type)) luaL_error(L, "unknown type name: %s", tn);
            read_patch_value(L, val_idx, type, out);
            return type;
        }
        return infer_set_type(L, val_idx, out, owned);
    }

    /// mod.patch.array_set(array, index, value[, type]) -> bool
    static int l_patch_array_set(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        const size_t raw_elem_bytes = patchlib_array_element_size(array);
        const size_t elem_bytes = array_element_bytes(array);
        const bool is_ref = array_is_reference(array, elem_bytes);
        std::vector<unsigned char> storage(elem_bytes >= 8 ? elem_bytes : 8, 0);
        void *buffer = storage.data();
        std::vector<patch_handle_t> owned;
        const patch_type_t type = read_element(L, 3, 4, buffer, owned);
        if (!is_array_type_compatible(raw_elem_bytes, is_ref, type)) {
            for (const patch_handle_t h: owned) patchlib_free(h);
            return luaL_error(L, "array element type mismatch");
        }
        const bool ok = patchlib_array_set(array, index, buffer);
        for (const patch_handle_t h: owned) patchlib_free(h);
        lua_pushboolean(L, ok);
        return 1;
    }

    /// mod.patch.array_fill(array, value[, type]) -> bool
    static int l_patch_array_fill(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t raw_elem_bytes = patchlib_array_element_size(array);
        const size_t elem_bytes = array_element_bytes(array);
        const bool is_ref = array_is_reference(array, elem_bytes);
        std::vector<unsigned char> storage(elem_bytes >= 8 ? elem_bytes : 8, 0);
        void *buffer = storage.data();
        std::vector<patch_handle_t> owned;
        const patch_type_t type = read_element(L, 2, 3, buffer, owned);
        if (!is_array_type_compatible(raw_elem_bytes, is_ref, type)) {
            for (const patch_handle_t h: owned) patchlib_free(h);
            return luaL_error(L, "array element type mismatch");
        }
        const bool ok = patchlib_array_fill(array, buffer);
        for (const patch_handle_t h: owned) patchlib_free(h);
        lua_pushboolean(L, ok);
        return 1;
    }

    /// mod.patch.array_create(size, elem_type) -> 数组句柄（GC 托管）
    /// elem_type 可为类型句柄、基础类型名（"int32"/"float"...）或类型全名
    static int l_patch_array_create(lua_State *L) {
        const lua_Integer size_in = luaL_checkinteger(L, 1);
        if (size_in < 0 || size_in > (1 << 20))
            return luaL_error(L, "array size out of range (0..1048576)");
        const size_t size = static_cast<size_t>(size_in);
        patch_handle_t elem_type = nullptr;
        if (lua_type(L, 2) == LUA_TLIGHTUSERDATA) {
            elem_type = to_handle(L, 2);
        } else {
            const char *tn = luaL_checkstring(L, 2);
            patch_type_t bt;
            if (type_from_name(tn, &bt)) elem_type = patchlib_get_basic_type(bt);
            if (!elem_type) elem_type = patchlib_type_get_type("", tn);
        }
        push_owned_handle(L, elem_type ? patchlib_array_create(size, elem_type) : nullptr);
        return 1;
    }

    /// mod.patch.array_at_raw(array, index, size) -> string(原始字节) | nil
    static int l_patch_array_at_raw(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        const size_t size = static_cast<size_t>(luaL_checkinteger(L, 3));
        const size_t elem = array_element_bytes(array);
        if (size == 0 || size > elem) return luaL_error(L, "bad size (1..%d)", (int) elem);
        std::vector<unsigned char> buf(elem, 0);
        if (!patchlib_array_at(array, index, buf.data())) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, reinterpret_cast<const char *>(buf.data()), size);
        return 1;
    }

    /// mod.patch.array_set_raw(array, index, bytes) -> bool
    static int l_patch_array_set_raw(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        size_t len = 0;
        const char *data = luaL_checklstring(L, 3, &len);
        const size_t elem = array_element_bytes(array);
        if (len == 0 || len > elem) return luaL_error(L, "bad size (1..%d)", (int) elem);
        std::vector<unsigned char> buf(elem, 0);
        std::memcpy(buf.data(), data, len);
        lua_pushboolean(L, patchlib_array_set(array, index, buf.data()));
        return 1;
    }

    /// mod.patch.property_get_method(prop) -> getter 方法句柄
    static int l_patch_property_get_method(lua_State *L) {
        push_handle(L, patchlib_property_get_get_method(to_handle(L, 1)));
        return 1;
    }

    /// mod.patch.property_set_method(prop) -> setter 方法句柄
    static int l_patch_property_set_method(lua_State *L) {
        push_handle(L, patchlib_property_get_set_method(to_handle(L, 1)));
        return 1;
    }

    /// mod.patch.get_method_by_names(type, name, {names...}) -> method 句柄 | nil
    static int l_patch_get_method_by_names(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const char *name = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TTABLE);
        const int n = static_cast<int>(luaL_len(L, 3));
        if (n < 0 || n > MAX_CALL_ARGS) return luaL_error(L, "bad param count");
        std::vector<std::string> store;
        store.reserve(n);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 3, i);
            store.emplace_back(luaL_checkstring(L, -1));
            lua_pop(L, 1);
        }
        std::vector<const char *> names;
        names.reserve(n);
        for (auto &s: store) names.push_back(s.c_str());
        push_handle(L, type ? patchlib_type_get_method_by_param_names(type, name, n, names.data()) : nullptr);
        return 1;
    }


void add_patch_array(lua_State *L) {
    static const luaL_Reg fns[] = {
            {"get_field_vec2", l_patch_field_get_vec2},
            {"set_field_vec2", l_patch_field_set_vec2},
            {"get_field_raw", l_patch_field_get_raw},
            {"set_field_raw", l_patch_field_set_raw},
            {"array_length", l_patch_array_length},
            {"array_at", l_patch_array_at},
            {"array_set", l_patch_array_set},
            {"array_fill", l_patch_array_fill},
            {"array_create", l_patch_array_create},
            {"array_at_raw", l_patch_array_at_raw},
            {"array_set_raw", l_patch_array_set_raw},
            {"property_get_method", l_patch_property_get_method},
            {"property_set_method", l_patch_property_set_method},
            {"get_method_by_names", l_patch_get_method_by_names},
            {nullptr, nullptr},
    };
    luaL_setfuncs(L, fns, 0);
}

} // namespace lualoader::lua_api
