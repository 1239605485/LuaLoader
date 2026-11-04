#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ========================================================================
    // 按值结构体参数调用（Android）：mod.patch.struct_arg + invoke_value_args
    // ========================================================================
#if defined(__ANDROID__)
    static constexpr const char *STRUCT_ARG_MT = "lualoader.struct_arg";
    static constexpr int MAX_STRUCT_FIELDS = 32;

    struct LuaStructArg {
        void *data;                 // 结构体本体（malloc）
        size_t data_size;
        int field_count;
        patch_type_t field_types[MAX_STRUCT_FIELDS];
        void *roots[MAX_STRUCT_FIELDS]; // PATCH_OBJECT 字段的 GC 强引用
    };

    static bool struct_field_layout(const patch_type_t t, size_t *out_sz, size_t *out_al) {
        switch (t) {
            case PATCH_BOOL:
            case PATCH_INT8:
            case PATCH_UINT8:
                *out_sz = 1;
                *out_al = 1;
                return true;
            case PATCH_INT16:
            case PATCH_UINT16:
            case PATCH_CHAR:
                *out_sz = 2;
                *out_al = 2;
                return true;
            case PATCH_INT32:
            case PATCH_UINT32:
            case PATCH_FLOAT:
                *out_sz = 4;
                *out_al = 4;
                return true;
            case PATCH_INT64:
            case PATCH_UINT64:
                *out_sz = 8;
                *out_al = alignof(int64_t);
                return true;
            case PATCH_DOUBLE:
                *out_sz = 8;
                *out_al = alignof(double);
                return true;
            case PATCH_POINTER:
            case PATCH_OBJECT:
                *out_sz = sizeof(void *);
                *out_al = alignof(void *);
                return true;
            default:
                return false;
        }
    }

    static bool compute_struct_layout(const patch_handle_t struct_type, size_t *out_size, size_t *out_align) {
        if (!struct_type) return false;
        tefstd_vector_t fields;
        if (!patchlib_type_get_fields(struct_type, false, &fields)) return false;

        size_t offset = 0, max_align = 1;
        bool ok = true;
        const size_t n = tefstd_vector_size(&fields);
        for (size_t i = 0; i < n; ++i) {
            const patch_handle_t fh = *static_cast<patch_handle_t *>(tefstd_vector_at(&fields, i));
            if (!fh) continue;
            if (patchlib_field_is_static(fh)) { patchlib_free(fh); continue; }
            const patch_type_t ft = patchlib_field_get_type(fh);
            size_t sz = 0, al = 1;
            bool field_ok = true;
            if (ft == PATCH_STRUCT) {
                const patch_handle_t sub = patchlib_field_get_type_handle(fh);
                if (!compute_struct_layout(sub, &sz, &al)) field_ok = false;
                patchlib_free(sub);
            } else if (!struct_field_layout(ft, &sz, &al)) {
                field_ok = false;
            }
            patchlib_free(fh);
            if (!field_ok) { ok = false; break; }
            offset = (offset + al - 1) & ~(al - 1);
            offset += sz;
            if (al > max_align) max_align = al;
        }
        tefstd_vector_destroy(&fields);
        if (!ok || offset == 0) return false;
        *out_size = (offset + max_align - 1) & ~(max_align - 1);
        *out_align = max_align;
        return true;
    }

    static int l_struct_arg_gc(lua_State *L) {
        auto *sa = static_cast<LuaStructArg *>(luaL_checkudata(L, 1, STRUCT_ARG_MT));
        for (int i = 0; i < sa->field_count; ++i) {
            if (sa->roots[i]) {
                patchlib_object_unroot(sa->roots[i]);
                sa->roots[i] = nullptr;
            }
        }
        if (sa->data) { free(sa->data); sa->data = nullptr; }
        return 0;
    }

    /// mod.patch.struct_arg({ "float", "float" }, { 1.5, 2.5 }) -> struct_arg
    static int l_patch_struct_arg(lua_State *L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        luaL_checktype(L, 2, LUA_TTABLE);
        const int n = static_cast<int>(luaL_len(L, 1));
        if (n <= 0 || n > MAX_STRUCT_FIELDS) return luaL_error(L, "bad field count");

        auto *sa = static_cast<LuaStructArg *>(lua_newuserdatauv(L, sizeof(LuaStructArg), n));
        memset(sa, 0, sizeof(*sa));
        sa->field_count = n;
        if (luaL_newmetatable(L, STRUCT_ARG_MT)) {
            lua_pushcfunction(L, l_struct_arg_gc);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);
        const int sa_idx = lua_gettop(L);

        // 解析字段类型并按自然对齐计算偏移
        size_t offset = 0, max_align = 1;
        for (int i = 0; i < n; ++i) {
            lua_rawgeti(L, 1, i + 1);
            patch_type_t t;
            if (!type_from_name(luaL_checkstring(L, -1), &t))
                return luaL_error(L, "unknown field type: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            sa->field_types[i] = t;

            size_t sz = 0, al = 1;
            if (!struct_field_layout(t, &sz, &al))
                return luaL_error(L, "unsupported field type");
            offset = (offset + al - 1) & ~(al - 1);
            offset += sz;
            if (al > max_align) max_align = al;
        }
        const size_t total = (offset + max_align - 1) & ~(max_align - 1);
        sa->data = malloc(total);
        if (!sa->data) return luaL_error(L, "struct_arg: out of memory");
        memset(sa->data, 0, total);

        // 逐字段按偏移写入（使用统一的尺寸/对齐规则，并为 object 字段保活）
        offset = 0;
        for (int i = 0; i < n; ++i) {
            size_t sz = 0, al = 1;
            struct_field_layout(sa->field_types[i], &sz, &al);
            offset = (offset + al - 1) & ~(al - 1);
            lua_rawgeti(L, 2, i + 1);
            void *slot = static_cast<char *>(sa->data) + offset;
            read_patch_value(L, -1, sa->field_types[i], slot);
            if (sa->field_types[i] == PATCH_OBJECT) {
                void *obj = *static_cast<void **>(slot);
                if (obj) sa->roots[i] = patchlib_object_root(obj);
                lua_setiuservalue(L, sa_idx, i + 1);
            } else {
                lua_pop(L, 1);
            }
            offset += sz;
        }

        sa->data_size = total;
        return 1;
    }

    /// mod.patch.invoke_value_args(method, instance, { arg... }) -> value | false
    /// 表中元素可以是：普通值（按签名编组），或 struct_arg 对象（按值结构体）
    static int l_patch_invoke_value_args(lua_State *L) {
        const patch_handle_t method = to_handle(L, 1);
        if (!method) return luaL_error(L, "invalid method handle");

        patch_method_signature_t sig;
        if (!patchlib_method_get_signature(method, &sig))
            return luaL_error(L, "cannot get method signature");

        patch_handle_t instance = nullptr;
        int arg_start = 2;
        if (sig.is_instance) {
            instance = to_handle(L, 2);
            arg_start = 3;
        }

        const int argc = static_cast<int>(tefstd_vector_size(&sig.arg_types));
        if (argc > MAX_CALL_ARGS) {
            patchlib_method_signature_free(&sig);
            return luaL_error(L, "too many method arguments (max %d)", MAX_CALL_ARGS);
        }
        if (sig.return_type == PATCH_STRUCT) {
            patchlib_method_signature_free(&sig);
            return luaL_error(L, "struct return type is not supported");
        }
        if (argc > 0 && !lua_istable(L, arg_start)) {
            patchlib_method_signature_free(&sig);
            return luaL_error(L, "argument table expected");
        }

        uint64_t storage[MAX_CALL_ARGS][1] = {};
        void *argv[MAX_CALL_ARGS] = {};

        // 逐个参数编组：struct_arg 用户数据必须匹配结构体参数签名及大小；
        // 其余参数按方法签名类型读值。
        for (int i = 0; i < argc; ++i) {
            const auto type = *static_cast<patch_type_t *>(tefstd_vector_at(&sig.arg_types, i));
            lua_rawgeti(L, arg_start, i + 1);
            if (auto *sa = static_cast<LuaStructArg *>(luaL_testudata(L, -1, STRUCT_ARG_MT))) {
                lua_pop(L, 1);
                if (type != PATCH_STRUCT) {
                    patchlib_method_signature_free(&sig);
                    return luaL_error(L, "argument %d is not a struct parameter", i + 1);
                }
                const auto *th = static_cast<patch_handle_t *>(tefstd_vector_at(&sig.arg_type_handles, i));
                size_t expected_size = 0, expected_align = 1;
                if (!th || !compute_struct_layout(*th, &expected_size, &expected_align) ||
                    sa->data_size != expected_size) {
                    patchlib_method_signature_free(&sig);
                    return luaL_error(L, "argument %d struct_arg size mismatch", i + 1);
                }
                argv[i] = sa->data;
            } else {
                const bool ok_val = (type != PATCH_STRUCT) && try_read_patch_value(L, -1, type, &storage[i]);
                lua_pop(L, 1);
                if (!ok_val) {
                    patchlib_method_signature_free(&sig);
                    return luaL_error(L, "argument %d type mismatch", i + 1);
                }
                argv[i] = &storage[i];
            }
        }

        uint64_t result = 0;
        const bool ok = patchlib_method_invoke_args(method, instance, &result, argv);
        const patch_type_t return_type = sig.return_type;
        patchlib_method_signature_free(&sig);
        if (!ok) {
            lua_pushboolean(L, false);
            return 1;
        }
        push_patch_value(L, return_type, &result);
        return 1;
    }
#else
    static int l_patch_struct_arg(lua_State *L) {
        return luaL_error(L, "struct_arg 仅 Android 可用");
    }
    static int l_patch_invoke_value_args(lua_State *L) {
        (void) L;
        lua_pushboolean(L, 0);
        return 1;
    }
#endif

    /// mod.patch.field_pointer(field, instance) -> lightuserdata | nil
    /// 仅 Android 暴露（原生字段真实指针）；桌面端返回 nil
    static int l_patch_field_pointer(lua_State *L) {
#if defined(__ANDROID__)
        const patch_handle_t f = to_handle(L, 1);
        if (!f || instance_missing(L, 2, f)) { lua_pushnil(L); return 1; }
        void *p = patchlib_field_get_pointer(f, to_handle(L, 2));
        if (p) lua_pushlightuserdata(L, p); else lua_pushnil(L);
#else
        (void) L;
        lua_pushnil(L);
#endif
        return 1;
    }

    /// mod.patch.field_is_reference(field) -> bool
    /// true 表示该字段是托管引用（数组/对象/字符串）；
    /// false 表示内联值类型（PE 上 Projectile.ai / localAI 就属于这种）。
    static int l_patch_field_is_reference(lua_State *L) {
        const patch_handle_t f = to_handle(L, 1);
        if (!f) { lua_pushboolean(L, 0); return 1; }
        lua_pushboolean(L, patchlib_field_get_type(f) == PATCH_OBJECT);
        return 1;
    }

    /// mod.patch.field_size(field) -> int（不可用时返回 0）
    static int l_patch_field_size(lua_State *L) {
        const patch_handle_t f = to_handle(L, 1);
        if (!f || !patchlib_field_get_size) {
            lua_pushinteger(L, 0);
            return 1;
        }
        lua_pushinteger(L, static_cast<lua_Integer>(patchlib_field_get_size(f)));
        return 1;
    }

    /// mod.patch.mem_read(ptr, offset, size) -> string | nil
    static int l_patch_mem_read(lua_State *L) {
        const void *base = lua_touserdata(L, 1);
        if (!base) {
            lua_pushnil(L);
            return 1;
        }
        const long offset = static_cast<long>(luaL_checkinteger(L, 2));
        const size_t size = static_cast<size_t>(luaL_checkinteger(L, 3));
        if (size == 0 || size > 4096) return luaL_error(L, "bad size (1..4096)");
        lua_pushlstring(L, static_cast<const char *>(base) + offset, size);
        return 1;
    }

    /// mod.patch.mem_write(ptr, offset, bytes) -> true
    static int l_patch_mem_write(lua_State *L) {
        void *base = lua_touserdata(L, 1);
        if (!base) return luaL_error(L, "invalid pointer");
        const long offset = static_cast<long>(luaL_checkinteger(L, 2));
        size_t len = 0;
        const char *data = luaL_checklstring(L, 3, &len);
        if (len == 0 || len > 4096) return luaL_error(L, "bad size (1..4096)");
        std::memcpy(static_cast<char *>(base) + offset, data, len);
        lua_pushboolean(L, true);
        return 1;
    }

    /// 基础标量/指针类型的字节大小（不依赖内核符号）
    size_t patch_scalar_size(const patch_type_t type) {
        switch (type) {
            case PATCH_BOOL:
            case PATCH_INT8:
            case PATCH_UINT8:
            case PATCH_CHAR:   return 1;
            case PATCH_INT16:
            case PATCH_UINT16: return 2;
            case PATCH_INT32:
            case PATCH_UINT32:
            case PATCH_FLOAT:  return 4;
            case PATCH_INT64:
            case PATCH_UINT64:
            case PATCH_DOUBLE:  return 8;
            case PATCH_POINTER:
            case PATCH_OBJECT: return sizeof(void *);
            default:           return 0;
        }
    }

    /// mod.patch.ptr_add(ptr, byte_offset) -> lightuserdata | nil
    static int l_patch_ptr_add(lua_State *L) {
        void *base = lua_touserdata(L, 1);
        if (!base) {
            lua_pushnil(L);
            return 1;
        }
        const long offset = static_cast<long>(luaL_checkinteger(L, 2));
        lua_pushlightuserdata(L, static_cast<char *>(base) + offset);
        return 1;
    }

    /// mod.patch.ptr_deref(ptr, byte_offset) -> lightuserdata | nil（读取该处的指针值）
    static int l_patch_ptr_deref(lua_State *L) {
        void *base = lua_touserdata(L, 1);
        if (!base) {
            lua_pushnil(L);
            return 1;
        }
        const long offset = static_cast<long>(luaL_checkinteger(L, 2));
        void *p = *reinterpret_cast<void **>(static_cast<char *>(base) + offset);
        if (p) lua_pushlightuserdata(L, p);
        else lua_pushnil(L);
        return 1;
    }

    /// mod.patch.mem_read_values(ptr, byte_offset, count, type) -> table
    /// 一次跨边界批量读取连续的同类型值（C 快通道的核心）
    static int l_patch_mem_read_values(lua_State *L) {
        const void *base = lua_touserdata(L, 1);
        if (!base) {
            lua_newtable(L);
            return 1;
        }
        const long offset = static_cast<long>(luaL_checkinteger(L, 2));
        const size_t count = static_cast<size_t>(luaL_checkinteger(L, 3));
        const char *tn = luaL_checkstring(L, 4);
        patch_type_t type;
        if (!type_from_name(tn, &type)) return luaL_error(L, "unknown type name: %s", tn);
        const size_t esz = patch_scalar_size(type);
        if (esz == 0) return luaL_error(L, "unsupported type for bulk read: %s", tn);
        if (count > (1u << 20)) return luaL_error(L, "count too large (max 1048576)");

        const char *p = static_cast<const char *>(base) + offset;
        lua_createtable(L, static_cast<int>(count), 0);
        for (size_t i = 0; i < count; ++i) {
            push_patch_value(L, type, p + i * esz);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    /// mod.patch.mem_write_values(ptr, byte_offset, table, type) -> count
    static int l_patch_mem_write_values(lua_State *L) {
        void *base = lua_touserdata(L, 1);
        if (!base) return luaL_error(L, "invalid pointer");
        const long offset = static_cast<long>(luaL_checkinteger(L, 2));
        luaL_checktype(L, 3, LUA_TTABLE);
        const char *tn = luaL_checkstring(L, 4);
        patch_type_t type;
        if (!type_from_name(tn, &type)) return luaL_error(L, "unknown type name: %s", tn);
        const size_t esz = patch_scalar_size(type);
        if (esz == 0) return luaL_error(L, "unsupported type for bulk write: %s", tn);
        const size_t count = static_cast<size_t>(luaL_len(L, 3));
        if (count > (1u << 20)) return luaL_error(L, "count too large (max 1048576)");

        char *p = static_cast<char *>(base) + offset;
        for (size_t i = 0; i < count; ++i) {
            lua_rawgeti(L, 3, static_cast<lua_Integer>(i + 1));
            read_patch_value(L, -1, type, p + i * esz);
            lua_pop(L, 1);
        }
        lua_pushinteger(L, static_cast<lua_Integer>(count));
        return 1;
    }


void add_patch_struct(lua_State *L) {
    static const luaL_Reg fns[] = {
            {"field_is_reference", l_patch_field_is_reference},
            {"struct_arg", l_patch_struct_arg},
            {"invoke_value_args", l_patch_invoke_value_args},
            {"field_pointer", l_patch_field_pointer},
            {"field_size", l_patch_field_size},
            {"mem_read", l_patch_mem_read},
            {"mem_write", l_patch_mem_write},
            {"ptr_add", l_patch_ptr_add},
            {"ptr_deref", l_patch_ptr_deref},
            {"mem_read_values", l_patch_mem_read_values},
            {"mem_write_values", l_patch_mem_write_values},
            {nullptr, nullptr},
    };
    luaL_setfuncs(L, fns, 0);
}

} // namespace lualoader::lua_api
