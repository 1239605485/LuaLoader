#include "api_internal.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace lualoader::lua_api {

    /// patchlib_string_cstr 在 Windows 上返回 .NET Marshal.AllocHGlobal 的内存
    /// （LocalAlloc），不能用 MinGW 的 free() 释放，否则会破坏堆。
    static void free_managed_cstr(char *p) {
        if (!p) return;
#if defined(_WIN32)
        LocalFree(static_cast<HLOCAL>(p));
#else
        free(p);
#endif
    }

    // ========================================================================
    // Patchlib: 类型 / 字段 / 方法
    // ========================================================================

    static int l_patch_get_type(lua_State *L) {
        const char *ns = luaL_checkstring(L, 1);
        const char *name = luaL_checkstring(L, 2);
        push_returned_object(L, patchlib_type_get_type(ns, name));
        return 1;
    }

    bool type_from_name(const char *name, patch_type_t *out) {
        static const struct {
            const char *name;
            patch_type_t type;
        } table[] = {
                {"void", PATCH_VOID}, {"int8", PATCH_INT8}, {"int16", PATCH_INT16}, {"int32", PATCH_INT32},
                {"int64", PATCH_INT64}, {"uint8", PATCH_UINT8}, {"uint16", PATCH_UINT16}, {"uint32", PATCH_UINT32},
                {"uint64", PATCH_UINT64}, {"bool", PATCH_BOOL}, {"float", PATCH_FLOAT}, {"double", PATCH_DOUBLE},
                {"pointer", PATCH_POINTER}, {"object", PATCH_OBJECT}, {"char", PATCH_CHAR},
        };
        for (const auto &entry: table) {
            if (std::strcmp(entry.name, name) == 0) {
                *out = entry.type;
                return true;
            }
        }
        return false;
    }

    /// 取字段类型：优先使用 Lua 显式传入的类型名，否则向内核查询
    static patch_type_t resolve_field_type(lua_State *L, const int type_idx, const patch_handle_t field) {
        if (!lua_isnoneornil(L, type_idx)) {
            if (lua_type(L, type_idx) == LUA_TSTRING) {
                patch_type_t type;
                if (type_from_name(lua_tostring(L, type_idx), &type)) return type;
                luaL_error(L, "unknown type name: %s", lua_tostring(L, type_idx));
            } else if (lua_isnumber(L, type_idx)) {
                return static_cast<patch_type_t>(lua_tointeger(L, type_idx));
            }
        }
        return patchlib_field_get_type(field);
    }

    static int l_patch_get_basic_type(lua_State *L) {
        const char *name = luaL_checkstring(L, 1);
        patch_type_t type;
        if (type_from_name(name, &type)) {
            push_returned_object(L, patchlib_get_basic_type(type));
        } else {
            lua_pushnil(L);
        }
        return 1;
    }

    static int l_patch_get_parent(lua_State *L) {
        push_returned_object(L, patchlib_type_get_parent(to_handle(L, 1)));
        return 1;
    }

    /// mod.patch.get_inner_type(parent, name) -> 嵌套类型句柄 | nil
    /// 例如：内层类 ItemID.Sets / PrefixLegacy.ItemSets 只能通过它获取
    /// （il2cpp_class_from_name(ns, "ItemID.Sets") 在 Android 上取不到）。
    static int l_patch_get_inner_type(lua_State *L) {
        const patch_handle_t parent = to_handle(L, 1);
        const char *name = luaL_checkstring(L, 2);
        push_returned_object(L, patchlib_type_get_inner_type(parent, name));
        return 1;
    }

    static int l_patch_type_name(lua_State *L) {
        const char *name = patchlib_type_get_name(to_handle(L, 1));
        if (name) lua_pushstring(L, name); else lua_pushnil(L);
        return 1;
    }

    static int l_patch_is_valid(lua_State *L) {
        lua_pushboolean(L, patchlib_is_valid(to_handle(L, 1)));
        return 1;
    }

    static int l_patch_get_field(lua_State *L) {
        push_returned_object(L, patchlib_type_get_field(to_handle(L, 1), luaL_checkstring(L, 2)));
        return 1;
    }

    static int l_patch_get_property(lua_State *L) {
        push_returned_object(L, patchlib_type_get_property(to_handle(L, 1), luaL_checkstring(L, 2)));
        return 1;
    }

    static int l_patch_get_method(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const char *name = luaL_checkstring(L, 2);
        if (lua_isnoneornil(L, 3)) {
            push_returned_object(L, patchlib_type_get_method(type, name));
        } else {
            const int argc = static_cast<int>(luaL_checkinteger(L, 3));
            push_returned_object(L, patchlib_type_get_method_by_param_count(type, name, argc));
        }
        return 1;
    }

    // ========================================================================
    // Patchlib: 托管字符串辅助
    // ========================================================================

    /// 是否为字符串伪类型名 "string"
    static bool is_string_type_arg(lua_State *L, const int idx) {
        return lua_type(L, idx) == LUA_TSTRING && std::strcmp(lua_tostring(L, idx), "string") == 0;
    }

    /// 读取引用类型字段的对象句柄（Android 走真实指针，桌面走 get_value）
    static patch_handle_t read_field_object(const patch_handle_t field, const patch_handle_t instance) {
        patch_handle_t obj = nullptr;
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (ptr) obj = *static_cast<patch_handle_t *>(ptr);
#else
        patchlib_field_get_value(field, instance, &obj);
#endif
        return obj;
    }

    /// 写入引用类型字段的对象句柄
    static void write_field_object(const patch_handle_t field, const patch_handle_t instance, const patch_handle_t obj) {
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (ptr) *static_cast<patch_handle_t *>(ptr) = obj;
#else
        patch_handle_t value = obj;
        patchlib_field_set_value(field, instance, &value);
#endif
    }

    /// 把托管 System.String 对象句柄压入 Lua（失败压 nil）
    void push_managed_string(lua_State *L, const patch_handle_t str) {
        if (!str) {
            lua_pushnil(L);
            return;
        }
        char *c = patchlib_string_cstr(str);
        if (!c) {
            lua_pushnil(L);
            return;
        }
        lua_pushstring(L, c);
        free_managed_cstr(c);
    }

    /// 非静态字段必须给 instance。
    /// 否则 Android 上 patchlib_field_get_pointer 会算出 NULL + offset（例如 0x12f），
    /// 解引用就是 SIGSEGV；桌面端会抛托管空引用异常。
    bool instance_missing(lua_State *L, const int idx, const patch_handle_t field) {
        /* 不能只看 Lua 的 nil：已 free 的 OwnedHandle、table、用户数据等
         * 也都不是有效实例，to_handle 会返回 nullptr。 */
        return to_handle(L, idx) == nullptr && !patchlib_field_is_static(field);
    }

    static int l_patch_field_get_value(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (instance_missing(L, 2, field)) {
            lua_pushnil(L);
            return 1;
        }
        if (is_string_type_arg(L, 3)) {
            const patch_handle_t s = read_field_object(field, instance);
            push_managed_string(L, s);
            free_returned_object(s);
            return 1;
        }
        const patch_type_t type = resolve_field_type(L, 3, field);
#if defined(__ANDROID__)
        // Android/IL2CPP 下 get_value 不可靠，直接用字段真实指针
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (!ptr) {
            lua_pushnil(L);
            return 1;
        }
        push_patch_value(L, type, ptr);
#else
        /* 字段可能大于 8 字节（结构体），按真实大小分配，避免栈越界 */
        const size_t fsz = patchlib_field_get_size ? patchlib_field_get_size(field) : 0;
        std::vector<unsigned char> buf(fsz > 8 ? fsz : 8, 0);
        patchlib_field_get_value(field, instance, buf.data());
        if (type == PATCH_OBJECT) {
            /* 包装层为引用字段分配了新 GCHandle，所有权交给 Lua GC */
            push_returned_object(L, *reinterpret_cast<patch_handle_t *>(buf.data()));
        } else {
            push_patch_value(L, type, buf.data());
        }
#endif
        return 1;
    }

    static int l_patch_field_set_value(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (instance_missing(L, 2, field)) return 0;
        if (is_string_type_arg(L, 4)) {
            const char *s = luaL_checkstring(L, 3);
            const patch_handle_t str = patchlib_string_create(s);
            if (!str) return luaL_error(L, "cannot create managed string");
            write_field_object(field, instance, str);
            patchlib_free(str);
            return 0;
        }
        const patch_type_t type = resolve_field_type(L, 4, field);
#if defined(__ANDROID__)
        // Android/IL2CPP 下 set_value 不可靠，直接写入字段真实指针
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (!ptr) return luaL_error(L, "cannot get field pointer");
        read_patch_value(L, 3, type, ptr);
#else
        const size_t fsz = patchlib_field_get_size ? patchlib_field_get_size(field) : 0;
        std::vector<unsigned char> buf(fsz > 8 ? fsz : 8, 0);
        read_patch_value(L, 3, type, buf.data());
        patchlib_field_set_value(field, instance, buf.data());
#endif
        return 0;
    }

    /// 依据签名把 Lua 参数编组为 patchlib 的 void** 参数数组。
    /// 不抛错：成功返回参数个数(>=0)，参数过多返回 -1，参数值不可转换返回 -2。
    /// 这样调用方可以先释放已分配的 signature 再 luaL_error，避免 longjmp 跳过释放。
    static int build_arg_array(lua_State *L, const int first, const patch_method_signature_t *sig,
                               uint64_t storage[][1], void *argv[], const int capacity) {
        const int argc = sig ? static_cast<int>(tefstd_vector_size(&sig->arg_types)) : 0;
        if (argc > capacity) {
            return -1;
        }
        for (int i = 0; i < argc; ++i) {
            const auto type = *static_cast<patch_type_t *>(tefstd_vector_at(&sig->arg_types, i));
            if (!try_read_patch_value(L, first + i, type, &storage[i])) {
                return -2;
            }
            argv[i] = &storage[i];
        }
        return argc;
    }

    static int l_patch_invoke(lua_State *L) {
        const patch_handle_t method = to_handle(L, 1);
        if (!method) return luaL_error(L, "invalid method handle");

        patch_method_signature_t sig;
        if (!patchlib_method_get_signature(method, &sig)) {
            return luaL_error(L, "cannot get method signature");
        }

        patch_handle_t instance = nullptr;
        int arg_start = 2;
        if (sig.is_instance) {
            instance = to_handle(L, 2);
            arg_start = 3;
        }
        if (sig.return_type == PATCH_STRUCT) {
            patchlib_method_signature_free(&sig);
            return luaL_error(L, "struct return type is not supported");
        }

        uint64_t storage[MAX_CALL_ARGS][1] = {};
        void *argv[MAX_CALL_ARGS] = {};
        const int built = build_arg_array(L, arg_start, &sig, storage, argv, MAX_CALL_ARGS);
        if (built < 0) {
            patchlib_method_signature_free(&sig);
            if (built == -1) return luaL_error(L, "too many method arguments (max %d)", MAX_CALL_ARGS);
            return luaL_error(L, "method argument type mismatch");
        }

        uint64_t result = 0;
        const bool ok = patchlib_method_invoke_args(method, instance, &result, argv);
        const patch_type_t return_type = sig.return_type;
        patchlib_method_signature_free(&sig);

        if (!ok) {
            lua_pushboolean(L, false);
            return 1;
        }
        if (return_type == PATCH_OBJECT)
            push_returned_object(L, *reinterpret_cast<patch_handle_t *>(&result));
        else
            push_patch_value(L, return_type, &result);
        return 1;
    }

    static int l_patch_free(lua_State *L) {
        if (auto *ud = static_cast<OwnedHandle *>(luaL_testudata(L, 1, OWNED_HANDLE_MT))) {
            release_owned_handle(ud);
            return 0;
        }
        /* lightuserdata 是借用句柄（hook 参数、数组元素、原生指针），不能在此释放 */
        return 0;
    }

    // ========================================================================
    // Patchlib: 带参构造
    // ========================================================================

    /// 解析构造函数并创建实例（参数从 first_arg 开始按签名编组）
    /// own_ctor: ctor 是否由本函数拥有（桌面端是新建 GCHandle，必须释放）
    static int construct_instance(lua_State *L, const patch_handle_t ctor, const int first_arg,
                                  const bool own_ctor) {
        auto release_ctor = [&]() {
            if (own_ctor) patchlib_free(ctor);
        };
        if (!ctor) {
            release_ctor();
            return luaL_error(L, "constructor not found");
        }

        patch_method_signature_t sig;
        if (!patchlib_method_get_signature(ctor, &sig)) {
            release_ctor();
            return luaL_error(L, "cannot get constructor signature");
        }
        uint64_t storage[MAX_CALL_ARGS][1] = {};
        void *argv[MAX_CALL_ARGS] = {};
        const int built = build_arg_array(L, first_arg, &sig, storage, argv, MAX_CALL_ARGS);
        if (built < 0) {
            patchlib_method_signature_free(&sig);
            release_ctor();
            if (built == -1) return luaL_error(L, "too many constructor arguments (max %d)", MAX_CALL_ARGS);
            return luaL_error(L, "constructor argument type mismatch");
        }
        patchlib_method_signature_free(&sig);

        patch_handle_t instance = nullptr;
        const bool ok = patchlib_constructor_invoke(ctor, &instance, argv);
        release_ctor();
        if (!ok) {
            lua_pushnil(L);
            return 1;
        }
        // 新实例句柄由 Lua GC 管理，避免 GCHandle 泄漏
        push_owned_handle(L, instance);
        return 1;
    }

    /// mod.patch.new_instance(type[, ...])：
    /// 无额外参数时走无参实例化（与旧版一致）；有参数时按 ".ctor" 带参构造
    static int l_patch_new_instance(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const int argc = lua_gettop(L) - 1;
        if (argc <= 0) {
            // 新实例句柄由 Lua GC 管理，避免 GCHandle 泄漏（与 construct 路径一致）
            push_owned_handle(L, patchlib_type_new_instance(type));
            return 1;
        }
        if (argc > MAX_CALL_ARGS) {
            return luaL_error(L, "too many constructor arguments (max %d)", MAX_CALL_ARGS);
        }
        const patch_handle_t ctor = patchlib_type_get_method_by_param_count(type, ".ctor", argc);
        return construct_instance(L, ctor, 2, true);
    }

    /// mod.patch.construct(ctor, ...)：使用显式构造函数句柄创建实例
    static int l_patch_construct(lua_State *L) {
        const patch_handle_t ctor = to_handle(L, 1);
        if (!ctor) return luaL_error(L, "invalid constructor handle");
        return construct_instance(L, ctor, 2, false);
    }

    // ========================================================================
    // Patchlib: 托管字符串
    // ========================================================================

    /// mod.patch.string_create(s) -> 托管字符串句柄（由 Lua GC 管理）
    static int l_patch_string_create(lua_State *L) {
        const char *s = luaL_checkstring(L, 1);
        push_owned_handle(L, patchlib_string_create(s));
        return 1;
    }

    /// mod.patch.string_value(handle|string) -> Lua 字符串
    static int l_patch_string_value(lua_State *L) {
        const int t = lua_type(L, 1);
        if (t == LUA_TSTRING || t == LUA_TNUMBER) {
            lua_pushvalue(L, 1);
            return 1;
        }
        push_managed_string(L, to_handle(L, 1));
        return 1;
    }

    /// mod.patch.string_empty(handle) -> bool
    static int l_patch_string_empty(lua_State *L) {
        lua_pushboolean(L, patchlib_string_empty(to_handle(L, 1)));
        return 1;
    }

    /// mod.patch.string_length(handle) -> int
    static int l_patch_string_length(lua_State *L) {
        lua_pushinteger(L, static_cast<lua_Integer>(patchlib_string_length(to_handle(L, 1))));
        return 1;
    }

    // ========================================================================
    // Patchlib: 句柄生命周期
    // ========================================================================

    /// mod.patch.retain(handle) -> 由 Lua GC 管理的句柄副本
    /// 用于把 hook 临时对象跨帧缓存，避免内核释放原句柄后失效（Android 上为同一句柄）
    static int l_patch_retain(lua_State *L) {
        const patch_handle_t h = to_handle(L, 1);
        if (!h) {
            lua_pushnil(L);
            return 1;
        }
        push_owned_handle(L, copy_handle(h));
        return 1;
    }


void add_patch_core(lua_State *L) {
    static const luaL_Reg fns[] = {
            {"get_type", l_patch_get_type},
            {"get_basic_type", l_patch_get_basic_type},
            {"new_instance", l_patch_new_instance},
            {"construct", l_patch_construct},
            {"get_parent", l_patch_get_parent},
            {"get_inner_type", l_patch_get_inner_type},
            {"type_name", l_patch_type_name},
            {"is_valid", l_patch_is_valid},
            {"get_field", l_patch_get_field},
            {"get_property", l_patch_get_property},
            {"get_method", l_patch_get_method},
            {"get_field_value", l_patch_field_get_value},
            {"set_field_value", l_patch_field_set_value},
            {"string_create", l_patch_string_create},
            {"string_value", l_patch_string_value},
            {"string_empty", l_patch_string_empty},
            {"string_length", l_patch_string_length},
            {"retain", l_patch_retain},
            {"copy", l_patch_retain},
            {"invoke", l_patch_invoke},
            {"free", l_patch_free},
            {nullptr, nullptr},
    };
    luaL_setfuncs(L, fns, 0);
}

} // namespace lualoader::lua_api
