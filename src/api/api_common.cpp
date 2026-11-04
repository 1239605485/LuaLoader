#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ========================================================================
    // 基础工具
    // ========================================================================

    lua_mod_handle_t *get_handle(lua_State *L) {
        lua_getfield(L, LUA_REGISTRYINDEX, HANDLE_KEY);
        auto *h = static_cast<lua_mod_handle_t *>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return h;
    }

    /// 复制内核对象句柄，使其生命周期独立于 hook 调用。
    /// 桌面端为新 GCHandle（指向同一对象，读写字段仍作用于原对象）；Android 直接返回原句柄，
    /// 由 push_owned_handle 建立 GC 强引用。
    patch_handle_t copy_handle(const patch_handle_t h) {
#if defined(__ANDROID__)
        return h;
#else
        return patchlib_handle_copy(h);
#endif
    }

    /// 释放托管句柄持有的内核资源：Android 解除 GC 强引用，桌面端释放 GCHandle
    void release_owned_handle(OwnedHandle *ud) {
        if (!ud->handle) return;
#if defined(__ANDROID__)
        patchlib_object_unroot(ud->root);
        ud->root = nullptr;
#else
        patchlib_free(ud->handle);
#endif
        ud->handle = nullptr;
    }

    /// 压入一个由 Lua GC 托管的句柄（nil 安全）。
    /// Android 句柄是裸对象指针，必须建立 GC 强引用，否则托管 GC 回收对象后该句柄悬空。
    void push_owned_handle(lua_State *L, const patch_handle_t h) {
        if (!h) {
            lua_pushnil(L);
            return;
        }
        auto *ud = static_cast<OwnedHandle *>(lua_newuserdatauv(L, sizeof(OwnedHandle), 0));
        ud->handle = h;
#if defined(__ANDROID__)
        ud->root = patchlib_object_root(h);
#else
        ud->root = nullptr;
#endif
        luaL_setmetatable(L, OWNED_HANDLE_MT);
    }

    /// 托管句柄的 __gc：Lua GC 回收时释放内核句柄，避免泄漏
    int l_owned_handle_gc(lua_State *L) {
        auto *ud = static_cast<OwnedHandle *>(luaL_checkudata(L, 1, OWNED_HANDLE_MT));
        release_owned_handle(ud);
        return 0;
    }

    /// 为注册 defaults 的引用字段保存来源值；数值字段不需要引用
    int ref_default_value(lua_State *L, const int idx, const patch_type_t type) {
        if (type != PATCH_OBJECT && type != PATCH_POINTER) return LUA_NOREF;
        lua_pushvalue(L, idx);
        return luaL_ref(L, LUA_REGISTRYINDEX);
    }

    void unref_default_values(lua_State *L, std::vector<LuaItemFieldValue> &defaults) {
        if (!L) return;
        for (auto &d: defaults) {
            if (d.ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, d.ref);
            d.ref = LUA_NOREF;
        }
    }

    void push_handle(lua_State *L, const patch_handle_t h) {
        if (h) {
            lua_pushlightuserdata(L, h);
        } else {
            lua_pushnil(L);
        }
    }

    /// 从 Lua 栈取句柄：兼容 lightuserdata（借用）与 GC 托管 userdata（自有）
    patch_handle_t to_handle(lua_State *L, const int idx) {
        if (lua_isnoneornil(L, idx)) return nullptr;
        if (lua_type(L, idx) == LUA_TLIGHTUSERDATA) {
            return static_cast<patch_handle_t>(lua_touserdata(L, idx));
        }
        if (auto *ud = static_cast<OwnedHandle *>(luaL_testudata(L, idx, OWNED_HANDLE_MT))) {
            return ud->handle;
        }
        return nullptr;
    }

    /// 将 patchlib 的值缓冲区压入 Lua 栈
    void push_patch_value(lua_State *L, const patch_type_t type, const void *value) {
        if (!value) {
            lua_pushnil(L);
            return;
        }
        switch (type) {
            case PATCH_BOOL:     lua_pushboolean(L, *static_cast<const bool *>(value)); break;
            case PATCH_INT8:     lua_pushinteger(L, *static_cast<const int8_t *>(value)); break;
            case PATCH_INT16:    lua_pushinteger(L, *static_cast<const int16_t *>(value)); break;
            case PATCH_INT32:    lua_pushinteger(L, *static_cast<const int32_t *>(value)); break;
            case PATCH_INT64:    lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<const int64_t *>(value))); break;
            case PATCH_UINT8:    lua_pushinteger(L, *static_cast<const uint8_t *>(value)); break;
            case PATCH_UINT16:   lua_pushinteger(L, *static_cast<const uint16_t *>(value)); break;
            case PATCH_UINT32:   lua_pushinteger(L, *static_cast<const uint32_t *>(value)); break;
            case PATCH_UINT64:   lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<const uint64_t *>(value))); break;
            case PATCH_FLOAT:    lua_pushnumber(L, *static_cast<const float *>(value)); break;
            case PATCH_DOUBLE:   lua_pushnumber(L, *static_cast<const double *>(value)); break;
            case PATCH_CHAR:     lua_pushinteger(L, *static_cast<const char *>(value)); break;
            case PATCH_POINTER:
            case PATCH_OBJECT: {
                void *ptr = *static_cast<void *const *>(value);
                if (ptr) lua_pushlightuserdata(L, ptr);
                else lua_pushnil(L);
                break;
            }
            case PATCH_VOID:
            default:
                lua_pushnil(L);
                break;
        }
    }

    /// 从 Lua 栈读取值写入 patchlib 值缓冲区，缓冲区至少 8 字节
    bool read_patch_value(lua_State *L, const int idx, const patch_type_t type, void *out) {
        switch (type) {
            case PATCH_BOOL:
                *static_cast<bool *>(out) = lua_toboolean(L, idx) != 0;
                break;
            case PATCH_INT8:
                *static_cast<int8_t *>(out) = static_cast<int8_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_INT16:
                *static_cast<int16_t *>(out) = static_cast<int16_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_INT32:
                *static_cast<int32_t *>(out) = static_cast<int32_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_INT64:
                *static_cast<int64_t *>(out) = static_cast<int64_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_UINT8:
                *static_cast<uint8_t *>(out) = static_cast<uint8_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_UINT16:
                *static_cast<uint16_t *>(out) = static_cast<uint16_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_UINT32:
                *static_cast<uint32_t *>(out) = static_cast<uint32_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_UINT64:
                *static_cast<uint64_t *>(out) = static_cast<uint64_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_FLOAT:
                *static_cast<float *>(out) = static_cast<float>(luaL_checknumber(L, idx));
                break;
            case PATCH_DOUBLE:
                *static_cast<double *>(out) = static_cast<double>(luaL_checknumber(L, idx));
                break;
            case PATCH_CHAR:
                *static_cast<char *>(out) = static_cast<char>(luaL_checkinteger(L, idx));
                break;
            case PATCH_POINTER:
            case PATCH_OBJECT:
                if (!lua_isnoneornil(L, idx) &&
                    lua_type(L, idx) != LUA_TLIGHTUSERDATA &&
                    !luaL_testudata(L, idx, OWNED_HANDLE_MT)) {
                    luaL_error(L, "handle or nil expected");
                }
                *static_cast<void **>(out) = to_handle(L, idx);
                break;
            case PATCH_VOID:
                break;
            default:
                luaL_error(L, "unsupported value type");
                break;
        }
        return true;
    }

    /// read_patch_value 的“不抛错”版本：值无法转换时返回 false，交由调用方自行清理后报错。
    /// 用于 `luaL_error`(longjmp) 会跳过 C++ 析构 / patchlib 释放的场景。
    bool try_read_patch_value(lua_State *L, const int idx, const patch_type_t type, void *out) {
        switch (type) {
            case PATCH_BOOL:
                *static_cast<bool *>(out) = lua_toboolean(L, idx) != 0;
                return true;
            case PATCH_INT8:
            case PATCH_INT16:
            case PATCH_INT32:
            case PATCH_INT64:
            case PATCH_UINT8:
            case PATCH_UINT16:
            case PATCH_UINT32:
            case PATCH_UINT64: {
                int isnum = 0;
                const lua_Integer v = lua_tointegerx(L, idx, &isnum);
                if (!isnum) return false;
                switch (type) {
                    case PATCH_INT8:   *static_cast<int8_t *>(out) = static_cast<int8_t>(v); break;
                    case PATCH_INT16:  *static_cast<int16_t *>(out) = static_cast<int16_t>(v); break;
                    case PATCH_INT32:  *static_cast<int32_t *>(out) = static_cast<int32_t>(v); break;
                    case PATCH_INT64:  *static_cast<int64_t *>(out) = static_cast<int64_t>(v); break;
                    case PATCH_UINT8:  *static_cast<uint8_t *>(out) = static_cast<uint8_t>(v); break;
                    case PATCH_UINT16: *static_cast<uint16_t *>(out) = static_cast<uint16_t>(v); break;
                    case PATCH_UINT32: *static_cast<uint32_t *>(out) = static_cast<uint32_t>(v); break;
                    default:           *static_cast<uint64_t *>(out) = static_cast<uint64_t>(v); break;
                }
                return true;
            }
            case PATCH_FLOAT:
            case PATCH_DOUBLE: {
                int isnum = 0;
                const lua_Number v = lua_tonumberx(L, idx, &isnum);
                if (!isnum) return false;
                if (type == PATCH_FLOAT) *static_cast<float *>(out) = static_cast<float>(v);
                else *static_cast<double *>(out) = static_cast<double>(v);
                return true;
            }
            case PATCH_CHAR: {
                int isnum = 0;
                const lua_Integer v = lua_tointegerx(L, idx, &isnum);
                if (!isnum) return false;
                *static_cast<char *>(out) = static_cast<char>(v);
                return true;
            }
            case PATCH_POINTER:
            case PATCH_OBJECT:
                if (!lua_isnoneornil(L, idx) &&
                    lua_type(L, idx) != LUA_TLIGHTUSERDATA &&
                    !luaL_testudata(L, idx, OWNED_HANDLE_MT)) {
                    return false;
                }
                *static_cast<void **>(out) = to_handle(L, idx);
                return true;
            case PATCH_VOID:
                return true;
            default:
                return false;
        }
    }

    static int lua_traceback(lua_State *L) {
        const char *msg = lua_tostring(L, 1);
        if (msg) {
            luaL_traceback(L, L, msg, 1);
        } else {
            lua_pushliteral(L, "<error object>");
        }
        return 1;
    }

    /// 调用栈顶的函数（函数与参数已按顺序入栈），带 traceback，出错时记录日志
    int pcall_log(lua_State *L, const int nargs, const int nresults, const char *what) {
        const int funcidx = lua_gettop(L) - nargs;
        lua_pushcfunction(L, lua_traceback);
        lua_insert(L, funcidx);
        const int status = lua_pcall(L, nargs, nresults, funcidx);
        lua_remove(L, funcidx);
        if (status != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_ERROR("[lua] {} failed: {}", what, err ? err : "unknown error");
            lua_pop(L, 1);
        }
        return status;
    }

    // ========================================================================
    // 日志 API
    // ========================================================================

    static mod_log_level_t parse_log_level(lua_State *L, const int idx) {
        if (lua_type(L, idx) == LUA_TNUMBER) {
            const int lv = static_cast<int>(lua_tointeger(L, idx));
            if (lv >= MOD_LOG_LEVEL_TRACE && lv <= MOD_LOG_LEVEL_FATAL) {
                return static_cast<mod_log_level_t>(lv);
            }
            return MOD_LOG_LEVEL_INFO;
        }
        const char *name = lua_tostring(L, idx);
        if (!name) return MOD_LOG_LEVEL_INFO;
        if (std::strcmp(name, "trace") == 0) return MOD_LOG_LEVEL_TRACE;
        if (std::strcmp(name, "debug") == 0) return MOD_LOG_LEVEL_DEBUG;
        if (std::strcmp(name, "info") == 0) return MOD_LOG_LEVEL_INFO;
        if (std::strcmp(name, "warn") == 0 || std::strcmp(name, "warning") == 0) return MOD_LOG_LEVEL_WARNING;
        if (std::strcmp(name, "error") == 0) return MOD_LOG_LEVEL_ERROR;
        if (std::strcmp(name, "critical") == 0) return MOD_LOG_LEVEL_CRITICAL;
        if (std::strcmp(name, "fatal") == 0) return MOD_LOG_LEVEL_FATAL;
        return MOD_LOG_LEVEL_INFO;
    }

    static std::string join_lua_args(lua_State *L, const int first) {
        const int top = lua_gettop(L);
        std::string out;
        for (int i = first; i <= top; ++i) {
            size_t len = 0;
            const char *s = luaL_tolstring(L, i, &len);
            if (i > first) out += ' ';
            if (s) out.append(s, len);
            lua_pop(L, 1);
        }
        return out;
    }

    static int log_impl(lua_State *L, const mod_log_level_t level, const int first_arg) {
        auto *handle = get_handle(L);
        const std::string message = join_lua_args(L, first_arg);
        const char *tag = handle ? handle->mod_id.c_str() : "lua";
        logger::write(level, tag, message.c_str());
        return 0;
    }

    int l_mod_log(lua_State *L) {
        const mod_log_level_t level = parse_log_level(L, 1);
        return log_impl(L, level, 2);
    }

#define LUALOADER_LOG_WRAPPER(fn_name, level_const)          \
    int fn_name(lua_State *L) {                       \
        return log_impl(L, level_const, 1);                  \
    }

    LUALOADER_LOG_WRAPPER(l_mod_trace, MOD_LOG_LEVEL_TRACE)
    LUALOADER_LOG_WRAPPER(l_mod_debug, MOD_LOG_LEVEL_DEBUG)
    LUALOADER_LOG_WRAPPER(l_mod_info, MOD_LOG_LEVEL_INFO)
    LUALOADER_LOG_WRAPPER(l_mod_warn, MOD_LOG_LEVEL_WARNING)
    LUALOADER_LOG_WRAPPER(l_mod_error, MOD_LOG_LEVEL_ERROR)
    LUALOADER_LOG_WRAPPER(l_mod_fatal, MOD_LOG_LEVEL_FATAL)

#undef LUALOADER_LOG_WRAPPER


} // namespace lualoader::lua_api
