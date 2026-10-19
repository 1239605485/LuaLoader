/*******************************************************************************
 * LuaLoader - lua mod api
 * Copyright (C) 2026 lzup333
 * Author: lzup333
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *******************************************************************************/

#include "lua_api.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <utility>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "logger.hpp"
#ifdef LUALOADER_GUI
#include "gui.hpp"
#endif

#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/patchlib/struct/dictionary.h"
#include "tefkernel/patchlib/struct/list.h"
#include "tefkernel/patchlib/struct/string.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/tefstd/vector.h"

#include "tefkernel/terraria/asset.h"
#include "tefkernel/terraria/item_manager.h"
#include "tefkernel/terraria/projectile_manager.h"
#include "tefkernel/terraria/language_manager.h"
#include "tefkernel/terraria/recipe_manager.h"
#include "tefkernel/terraria/texture2d.h"

#include "image_decode.hpp"

namespace lualoader::lua_api {

    static constexpr const char *HANDLE_KEY = "lualoader.mod.handle";
    static constexpr const char *HOOK_METATABLE = "LuaLoader.Hook";
    static constexpr const char *OWNED_HANDLE_MT = "LuaLoader.OwnedHandle";
    static constexpr int MAX_HOOK_SLOTS = 1024;
    static constexpr int MAX_CALL_ARGS = 16;

    // ========================================================================
    // 基础工具
    // ========================================================================

    static lua_mod_handle_t *get_handle(lua_State *L) {
        lua_getfield(L, LUA_REGISTRYINDEX, HANDLE_KEY);
        auto *h = static_cast<lua_mod_handle_t *>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return h;
    }

    /// 由 Lua GC 托管的内核对象句柄：__gc 时自动释放（Android 上释放为空操作）
    struct OwnedHandle {
        patch_handle_t handle;
    };

    /// 复制内核对象句柄，使其生命周期独立于 hook 调用。
    /// 桌面端为新 GCHandle（指向同一对象，读写字段仍作用于原对象）；Android 直接返回原句柄。
    static patch_handle_t copy_handle(const patch_handle_t h) {
#if defined(__ANDROID__)
        return h;
#else
        return patchlib_handle_copy(h);
#endif
    }

    /// 压入一个由 Lua GC 托管的句柄（nil 安全）
    static void push_owned_handle(lua_State *L, const patch_handle_t h) {
        if (!h) {
            lua_pushnil(L);
            return;
        }
        auto *ud = static_cast<OwnedHandle *>(lua_newuserdatauv(L, sizeof(OwnedHandle), 0));
        ud->handle = h;
        luaL_setmetatable(L, OWNED_HANDLE_MT);
    }

    /// 托管句柄的 __gc：Lua GC 回收时释放内核句柄，避免泄漏
    static int l_owned_handle_gc(lua_State *L) {
        auto *ud = static_cast<OwnedHandle *>(luaL_checkudata(L, 1, OWNED_HANDLE_MT));
        if (ud && ud->handle) {
            patchlib_free(ud->handle);
            ud->handle = nullptr;
        }
        return 0;
    }

    static void push_handle(lua_State *L, const patch_handle_t h) {
        if (h) {
            lua_pushlightuserdata(L, h);
        } else {
            lua_pushnil(L);
        }
    }

    /// 从 Lua 栈取句柄：兼容 lightuserdata（借用）与 GC 托管 userdata（自有）
    static patch_handle_t to_handle(lua_State *L, const int idx) {
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
    static void push_patch_value(lua_State *L, const patch_type_t type, const void *value) {
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
    static bool read_patch_value(lua_State *L, const int idx, const patch_type_t type, void *out) {
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
                *static_cast<void **>(out) = to_handle(L, idx);
                break;
            case PATCH_VOID:
            default:
                break;
        }
        return true;
    }

    /// read_patch_value 的“不抛错”版本：值无法转换时返回 false，交由调用方自行清理后报错。
    /// 用于 `luaL_error`(longjmp) 会跳过 C++ 析构 / patchlib 释放的场景。
    static bool try_read_patch_value(lua_State *L, const int idx, const patch_type_t type, void *out) {
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
                *static_cast<void **>(out) = to_handle(L, idx);
                return true;
            case PATCH_VOID:
            default:
                return true;
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
    static int pcall_log(lua_State *L, const int nargs, const int nresults, const char *what) {
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

    static int l_mod_log(lua_State *L) {
        const mod_log_level_t level = parse_log_level(L, 1);
        return log_impl(L, level, 2);
    }

#define LUALOADER_LOG_WRAPPER(fn_name, level_const)          \
    static int fn_name(lua_State *L) {                       \
        return log_impl(L, level_const, 1);                  \
    }

    LUALOADER_LOG_WRAPPER(l_mod_trace, MOD_LOG_LEVEL_TRACE)
    LUALOADER_LOG_WRAPPER(l_mod_debug, MOD_LOG_LEVEL_DEBUG)
    LUALOADER_LOG_WRAPPER(l_mod_info, MOD_LOG_LEVEL_INFO)
    LUALOADER_LOG_WRAPPER(l_mod_warn, MOD_LOG_LEVEL_WARNING)
    LUALOADER_LOG_WRAPPER(l_mod_error, MOD_LOG_LEVEL_ERROR)
    LUALOADER_LOG_WRAPPER(l_mod_fatal, MOD_LOG_LEVEL_FATAL)

#undef LUALOADER_LOG_WRAPPER

    // ========================================================================
    // 文件 API（限制在 private_dir 内）
    // ========================================================================

    static bool is_safe_relative(const char *name) {
        if (!name || name[0] == '\0') return false;
        if (name[0] == '/' || name[0] == '\\') return false;
        const std::filesystem::path p(name);
        if (p.is_absolute()) return false;
        for (const auto &part: p) {
            if (part == "..") return false;
        }
        return true;
    }

    static int l_mod_read_file(lua_State *L) {
        auto *handle = get_handle(L);
        const char *name = luaL_checkstring(L, 1);
        if (!handle) return luaL_error(L, "no mod context");
        if (!is_safe_relative(name)) {
            lua_pushnil(L);
            lua_pushfstring(L, "unsafe path: %s", name);
            return 2;
        }
        const std::filesystem::path path = std::filesystem::path(handle->private_dir) / name;
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            lua_pushnil(L);
            lua_pushfstring(L, "cannot open %s", name);
            return 2;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string data = ss.str();
        lua_pushlstring(L, data.data(), data.size());
        return 1;
    }

    static int l_mod_write_file(lua_State *L) {
        auto *handle = get_handle(L);
        const char *name = luaL_checkstring(L, 1);
        size_t len = 0;
        const char *data = luaL_checklstring(L, 2, &len);
        if (!handle) return luaL_error(L, "no mod context");
        if (!is_safe_relative(name)) {
            lua_pushnil(L);
            lua_pushfstring(L, "unsafe path: %s", name);
            return 2;
        }
        const std::filesystem::path path = std::filesystem::path(handle->private_dir) / name;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            lua_pushnil(L);
            lua_pushfstring(L, "cannot write %s", name);
            return 2;
        }
        file.write(data, static_cast<std::streamsize>(len));
        lua_pushboolean(L, file.good());
        return 1;
    }

    static int l_mod_file_exists(lua_State *L) {
        auto *handle = get_handle(L);
        const char *name = luaL_checkstring(L, 1);
        if (!handle) return luaL_error(L, "no mod context");
        if (!is_safe_relative(name)) {
            lua_pushboolean(L, false);
            return 1;
        }
        const std::filesystem::path path = std::filesystem::path(handle->private_dir) / name;
        lua_pushboolean(L, std::filesystem::exists(path));
        return 1;
    }

    // ========================================================================
    // Patchlib: 类型 / 字段 / 方法
    // ========================================================================

    static int l_patch_get_type(lua_State *L) {
        const char *ns = luaL_checkstring(L, 1);
        const char *name = luaL_checkstring(L, 2);
        push_handle(L, patchlib_type_get_type(ns, name));
        return 1;
    }

    static bool type_from_name(const char *name, patch_type_t *out) {
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
            push_handle(L, patchlib_get_basic_type(type));
        } else {
            lua_pushnil(L);
        }
        return 1;
    }

    static int l_patch_get_parent(lua_State *L) {
        push_handle(L, patchlib_type_get_parent(to_handle(L, 1)));
        return 1;
    }

    /// mod.patch.get_inner_type(parent, name) -> 嵌套类型句柄 | nil
    /// 例如：内层类 ItemID.Sets / PrefixLegacy.ItemSets 只能通过它获取
    /// （il2cpp_class_from_name(ns, "ItemID.Sets") 在 Android 上取不到）。
    static int l_patch_get_inner_type(lua_State *L) {
        const patch_handle_t parent = to_handle(L, 1);
        const char *name = luaL_checkstring(L, 2);
        push_handle(L, patchlib_type_get_inner_type(parent, name));
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
        push_handle(L, patchlib_type_get_field(to_handle(L, 1), luaL_checkstring(L, 2)));
        return 1;
    }

    static int l_patch_get_property(lua_State *L) {
        push_handle(L, patchlib_type_get_property(to_handle(L, 1), luaL_checkstring(L, 2)));
        return 1;
    }

    static int l_patch_get_method(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const char *name = luaL_checkstring(L, 2);
        if (lua_isnoneornil(L, 3)) {
            push_handle(L, patchlib_type_get_method(type, name));
        } else {
            const int argc = static_cast<int>(luaL_checkinteger(L, 3));
            push_handle(L, patchlib_type_get_method_by_param_count(type, name, argc));
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
    static void push_managed_string(lua_State *L, const patch_handle_t str) {
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
        free(c);
    }

    static int l_patch_field_get_value(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (is_string_type_arg(L, 3)) {
            push_managed_string(L, read_field_object(field, instance));
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
        uint64_t buffer = 0;
        patchlib_field_get_value(field, instance, &buffer);
        push_patch_value(L, type, &buffer);
#endif
        return 1;
    }

    static int l_patch_field_set_value(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
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
        uint64_t buffer = 0;
        read_patch_value(L, 3, type, &buffer);
        patchlib_field_set_value(field, instance, &buffer);
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
        push_patch_value(L, return_type, &result);
        return 1;
    }

    static int l_patch_free(lua_State *L) {
        if (auto *ud = static_cast<OwnedHandle *>(luaL_testudata(L, 1, OWNED_HANDLE_MT))) {
            if (ud->handle) {
                patchlib_free(ud->handle);
                ud->handle = nullptr;
            }
            return 0;
        }
        patchlib_free(to_handle(L, 1));
        return 0;
    }

    // ========================================================================
    // Patchlib: 带参构造
    // ========================================================================

    /// 解析构造函数并创建实例（参数从 first_arg 开始按签名编组）
    static int construct_instance(lua_State *L, const patch_handle_t ctor, const int first_arg) {
        if (!ctor) return luaL_error(L, "constructor not found");

        patch_method_signature_t sig;
        if (!patchlib_method_get_signature(ctor, &sig)) {
            return luaL_error(L, "cannot get constructor signature");
        }
        uint64_t storage[MAX_CALL_ARGS][1] = {};
        void *argv[MAX_CALL_ARGS] = {};
        const int built = build_arg_array(L, first_arg, &sig, storage, argv, MAX_CALL_ARGS);
        if (built < 0) {
            patchlib_method_signature_free(&sig);
            if (built == -1) return luaL_error(L, "too many constructor arguments (max %d)", MAX_CALL_ARGS);
            return luaL_error(L, "constructor argument type mismatch");
        }
        patchlib_method_signature_free(&sig);

        patch_handle_t instance = nullptr;
        if (!patchlib_constructor_invoke(ctor, &instance, argv)) {
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
        return construct_instance(L, ctor, 2);
    }

    /// mod.patch.construct(ctor, ...)：使用显式构造函数句柄创建实例
    static int l_patch_construct(lua_State *L) {
        const patch_handle_t ctor = to_handle(L, 1);
        if (!ctor) return luaL_error(L, "invalid constructor handle");
        return construct_instance(L, ctor, 2);
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

    /// 数组取元素。第三个参数为元素类型名，省略时按对象(指针)处理。
    static int l_patch_array_at(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        uint64_t buffer = 0;
        if (!patchlib_array_at(array, index, &buffer)) {
            lua_pushnil(L);
            return 1;
        }
        if (lua_isnoneornil(L, 3)) {
            void *ptr = *reinterpret_cast<void **>(&buffer);
            if (ptr) lua_pushlightuserdata(L, ptr);
            else lua_pushnil(L);
            return 1;
        }
        const char *type_name = luaL_checkstring(L, 3);
        if (std::strcmp(type_name, "string") == 0) {
            push_managed_string(L, *reinterpret_cast<void **>(&buffer));
            return 1;
        }
        patch_type_t type;
        if (!type_from_name(type_name, &type)) {
            return luaL_error(L, "unknown type name: %s", type_name);
        }
        push_patch_value(L, type, &buffer);
        return 1;
    }

    // ========================================================================
    // Patchlib: 数组写入 / 创建 / 原始字节 / 属性桥接 / 按名选重载
    // ========================================================================

    /// 依据 Lua 值推断写入类型（显式类型由调用方给出）
    static patch_type_t infer_set_type(lua_State *L, const int idx, void *out, std::vector<patch_handle_t> &owned) {
        const int t = lua_type(L, idx);
        if (t == LUA_TBOOLEAN) return PATCH_BOOL;
        if (t == LUA_TNUMBER) return lua_isinteger(L, idx) ? PATCH_INT32 : PATCH_FLOAT;
        if (t == LUA_TLIGHTUSERDATA) {
            *static_cast<void **>(out) = lua_touserdata(L, idx);
            return PATCH_OBJECT;
        }
        if (t == LUA_TSTRING) {
            patch_handle_t s = patchlib_string_create(lua_tostring(L, idx));
            owned.push_back(s);
            *static_cast<void **>(out) = s;
            return PATCH_OBJECT;
        }
        *static_cast<void **>(out) = nullptr;
        return PATCH_OBJECT;
    }

    /// 计算单个元素的写入缓冲；type_idx 为显式类型参数位置（无则按值推断）
    static void read_element(lua_State *L, const int val_idx, const int type_idx, void *out,
                             std::vector<patch_handle_t> &owned) {
        if (!lua_isnoneornil(L, type_idx)) {
            const char *tn = luaL_checkstring(L, type_idx);
            if (std::strcmp(tn, "string") == 0 || std::strcmp(tn, "object") == 0) {
                if (lua_type(L, val_idx) == LUA_TSTRING) {
                    patch_handle_t s = patchlib_string_create(lua_tostring(L, val_idx));
                    owned.push_back(s);
                    *static_cast<void **>(out) = s;
                } else {
                    *static_cast<void **>(out) = lua_touserdata(L, val_idx);
                }
                return;
            }
            patch_type_t type;
            if (!type_from_name(tn, &type)) luaL_error(L, "unknown type name: %s", tn);
            read_patch_value(L, val_idx, type, out);
            return;
        }
        infer_set_type(L, val_idx, out, owned);
    }

    /// mod.patch.array_set(array, index, value[, type]) -> bool
    static int l_patch_array_set(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        uint64_t buffer = 0;
        std::vector<patch_handle_t> owned;
        read_element(L, 3, 4, &buffer, owned);
        const bool ok = patchlib_array_set(array, index, &buffer);
        for (const patch_handle_t h: owned) patchlib_free(h);
        lua_pushboolean(L, ok);
        return 1;
    }

    /// mod.patch.array_fill(array, value[, type]) -> bool
    static int l_patch_array_fill(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        uint64_t buffer = 0;
        std::vector<patch_handle_t> owned;
        read_element(L, 2, 3, &buffer, owned);
        const bool ok = patchlib_array_fill(array, &buffer);
        for (const patch_handle_t h: owned) patchlib_free(h);
        lua_pushboolean(L, ok);
        return 1;
    }

    /// mod.patch.array_create(size, elem_type) -> 数组句柄（GC 托管）
    /// elem_type 可为类型句柄、基础类型名（"int32"/"float"...）或类型全名
    static int l_patch_array_create(lua_State *L) {
        const size_t size = static_cast<size_t>(luaL_checkinteger(L, 1));
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
        if (size == 0 || size > 64) return luaL_error(L, "bad size (1..64)");
        unsigned char buf[64];
        std::memset(buf, 0, size);
        if (!patchlib_array_at(array, index, buf)) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, reinterpret_cast<const char *>(buf), size);
        return 1;
    }

    /// mod.patch.array_set_raw(array, index, bytes) -> bool
    static int l_patch_array_set_raw(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        size_t len = 0;
        const char *data = luaL_checklstring(L, 3, &len);
        if (len == 0 || len > 64) return luaL_error(L, "bad size (1..64)");
        unsigned char buf[64];
        std::memcpy(buf, data, len);
        lua_pushboolean(L, patchlib_array_set(array, index, buf));
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
            push_handle(L, slot ? *slot : nullptr);
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
        push_handle(L, type ? patchlib_type_get_method_by_param_types(type, name, n, types) : nullptr);
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
        push_handle(L, type ? patchlib_type_get_method_by_signature(
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
        push_handle(L, def ? patchlib_type_make_generic_type(def, &vec) : nullptr);
        tefstd_vector_destroy(&vec);
        return 1;
    }

    /// mod.patch.make_generic_instance(method, { type, ... }) -> method | nil
    static int l_patch_make_generic_instance(lua_State *L) {
        const patch_handle_t m = to_handle(L, 1);
        tefstd_vector_t vec;
        if (!fill_type_vector(L, 2, &vec)) { lua_pushnil(L); return 1; }
        push_handle(L, m ? patchlib_method_make_generic_instance(m, &vec) : nullptr);
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
        push_handle(L, list ? patchlib_list_get_array(list) : nullptr);
        return 1;
    }

    // ========================================================================
    // 按值结构体参数调用（Android）：mod.patch.struct_arg + invoke_value_args
    // ========================================================================
#if defined(__ANDROID__)
    static constexpr const char *STRUCT_ARG_MT = "lualoader.struct_arg";
    static constexpr int MAX_STRUCT_FIELDS = 32;

    struct LuaStructArg {
        void *data;                 // 结构体本体（malloc）
        size_t data_size;
        patch_type_t field_types[MAX_STRUCT_FIELDS];
    };

    static int l_struct_arg_gc(lua_State *L) {
        auto *sa = static_cast<LuaStructArg *>(luaL_checkudata(L, 1, STRUCT_ARG_MT));
        if (sa->data) { free(sa->data); sa->data = nullptr; }
        return 0;
    }

    /// mod.patch.struct_arg({ "float", "float" }, { 1.5, 2.5 }) -> struct_arg
    static int l_patch_struct_arg(lua_State *L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        luaL_checktype(L, 2, LUA_TTABLE);
        const int n = static_cast<int>(luaL_len(L, 1));
        if (n <= 0 || n > MAX_STRUCT_FIELDS) return luaL_error(L, "bad field count");

        auto *sa = static_cast<LuaStructArg *>(lua_newuserdatauv(L, sizeof(LuaStructArg), 0));
        memset(sa, 0, sizeof(*sa));
        if (luaL_newmetatable(L, STRUCT_ARG_MT)) {
            lua_pushcfunction(L, l_struct_arg_gc);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);

        // 解析字段类型并按自然对齐计算偏移
        size_t offset = 0, max_align = 1;
        for (int i = 0; i < n; ++i) {
            lua_rawgeti(L, 1, i + 1);
            patch_type_t t;
            if (!type_from_name(luaL_checkstring(L, -1), &t))
                return luaL_error(L, "unknown field type: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            sa->field_types[i] = t;

            size_t sz = 8, al = 8;
            switch (t) {
                case PATCH_BOOL: case PATCH_INT8: case PATCH_UINT8: sz = 1; al = 1; break;
                case PATCH_INT16: case PATCH_UINT16: case PATCH_CHAR: sz = 2; al = 2; break;
                case PATCH_INT32: case PATCH_UINT32: case PATCH_FLOAT: sz = 4; al = 4; break;
                case PATCH_INT64: case PATCH_UINT64: case PATCH_DOUBLE: sz = 8; al = 8; break;
                case PATCH_POINTER: case PATCH_OBJECT: sz = sizeof(void *); al = alignof(void *); break;
                default: return luaL_error(L, "unsupported field type");
            }
            offset = (offset + al - 1) & ~(al - 1);
            offset += sz;
            if (al > max_align) max_align = al;
        }
        size_t total = (offset + max_align - 1) & ~(max_align - 1);
        sa->data = malloc(total);
        if (!sa->data) return luaL_error(L, "struct_arg: out of memory");
        memset(sa->data, 0, total);

        // 逐字段按偏移写入
        offset = 0;
        for (int i = 0; i < n; ++i) {
            const size_t al = (sa->field_types[i] == PATCH_INT32 || sa->field_types[i] == PATCH_FLOAT ||
                               sa->field_types[i] == PATCH_UINT32) ? 4 :
                              (sa->field_types[i] == PATCH_BOOL || sa->field_types[i] == PATCH_INT8 ||
                               sa->field_types[i] == PATCH_UINT8) ? 1 :
                              (sa->field_types[i] == PATCH_INT16 || sa->field_types[i] == PATCH_UINT16 ||
                               sa->field_types[i] == PATCH_CHAR) ? 2 : 8;
            offset = (offset + al - 1) & ~(al - 1);
            lua_rawgeti(L, 2, i + 1);
            read_patch_value(L, -1, sa->field_types[i],
                             static_cast<char *>(sa->data) + offset);
            lua_pop(L, 1);
            offset += (sa->field_types[i] == PATCH_INT64 || sa->field_types[i] == PATCH_UINT64 ||
                       sa->field_types[i] == PATCH_DOUBLE) ? 8 :
                      (sa->field_types[i] == PATCH_INT32 || sa->field_types[i] == PATCH_FLOAT ||
                       sa->field_types[i] == PATCH_UINT32) ? 4 :
                      (sa->field_types[i] == PATCH_INT16 || sa->field_types[i] == PATCH_UINT16 ||
                       sa->field_types[i] == PATCH_CHAR) ? 2 :
                      (sa->field_types[i] == PATCH_BOOL || sa->field_types[i] == PATCH_INT8 ||
                       sa->field_types[i] == PATCH_UINT8) ? 1 : sizeof(void *);
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
        uint64_t storage[MAX_CALL_ARGS][1] = {};
        void *argv[MAX_CALL_ARGS] = {};

        // 逐个参数编组：struct_arg 用户数据直接给出结构体字节；
        // 其余参数按方法签名类型读值。按值结构体由内核依据方法签名自动处理。
        for (int i = 0; i < argc && i < MAX_CALL_ARGS; ++i) {
            lua_rawgeti(L, arg_start, i + 1);
            if (auto *sa = static_cast<LuaStructArg *>(luaL_testudata(L, -1, STRUCT_ARG_MT))) {
                argv[i] = sa->data;
            } else {
                const auto type = *static_cast<patch_type_t *>(tefstd_vector_at(&sig.arg_types, i));
                if (!try_read_patch_value(L, -1, type, &storage[i])) {
                    lua_pop(L, 1);
                    patchlib_method_signature_free(&sig);
                    return luaL_error(L, "argument %d type mismatch", i + 1);
                }
                argv[i] = &storage[i];
            }
            lua_pop(L, 1);
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
        void *p = patchlib_field_get_pointer(to_handle(L, 1), to_handle(L, 2));
        if (p) lua_pushlightuserdata(L, p); else lua_pushnil(L);
#else
        (void) L;
        lua_pushnil(L);
#endif
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
    static size_t patch_scalar_size(const patch_type_t type) {
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
            case PATCH_DOUBLE:
            case PATCH_POINTER:
            case PATCH_OBJECT: return 8;
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

    // ========================================================================
    // Patchlib: 钩子（Prefix / Postfix）
    // ========================================================================

    struct HookSlot {
        bool used{false};
        lua_State *L{nullptr};
        lua_mod_handle_t *handle{nullptr};
        int prefix_ref{LUA_NOREF};
        int postfix_ref{LUA_NOREF};
        bool copy_handles{false}; ///< true 时把 instance/对象参数/返回值复制为 GC 托管句柄
        bool override_result{false}; ///< true 时允许 postfix 返回值覆盖原方法返回值（默认关闭，保证兼容）
        patch_hook_id_t hook_id{PATCH_HOOK_INVALID_ID};
    };

    static HookSlot g_hook_slots[MAX_HOOK_SLOTS];

    struct HookUserdata {
        int slot;
    };

    /// 钩子回调：先声明分发函数，再由宏生成固定签名的 C 入口
    static bool hook_prefix_dispatch(int slot, patch_handle_t instance, void **args,
                                     const patch_method_signature_t *sig, void *result);

    static void hook_postfix_dispatch(int slot, patch_handle_t instance, void **args, void *result,
                                      const patch_method_signature_t *sig);

    /// 跳板：按槽位号生成固定签名的 C 入口（用模板展开，数量随 MAX_HOOK_SLOTS 走）
    template<int N>
    static bool prefix_trampoline(patch_handle_t instance, void **args,
                                  const patch_method_signature_t *sig, void *result) {
        return hook_prefix_dispatch(N, instance, args, sig, result);
    }

    template<int N>
    static void postfix_trampoline(patch_handle_t instance, void **args, void *result,
                                   const patch_method_signature_t *sig) {
        hook_postfix_dispatch(N, instance, args, result, sig);
    }

    template<int... Is>
    static std::array<prefix_callback_t, sizeof...(Is)> make_prefix_table(std::integer_sequence<int, Is...>) {
        return {&prefix_trampoline<Is>...};
    }

    template<int... Is>
    static std::array<postfix_callback_t, sizeof...(Is)> make_postfix_table(std::integer_sequence<int, Is...>) {
        return {&postfix_trampoline<Is>...};
    }

    static const std::array<prefix_callback_t, MAX_HOOK_SLOTS> g_prefix_table =
            make_prefix_table(std::make_integer_sequence<int, MAX_HOOK_SLOTS>{});
    static const std::array<postfix_callback_t, MAX_HOOK_SLOTS> g_postfix_table =
            make_postfix_table(std::make_integer_sequence<int, MAX_HOOK_SLOTS>{});

    /// 压入一个参数值；copy_handles 时把对象/指针句柄复制为 GC 托管句柄
    static void push_hook_arg_value(lua_State *L, const patch_type_t type, const void *value, const bool copy_handles) {
        if (copy_handles && (type == PATCH_OBJECT || type == PATCH_POINTER) && value) {
            auto *ptr = *static_cast<void *const *>(value);
            if (ptr) {
                push_owned_handle(L, copy_handle(ptr));
                return;
            }
        }
        push_patch_value(L, type, value);
    }

    /// 钩子回调参数：instance、参数表 args（数组）、返回值 result
    static void push_hook_args(lua_State *L, patch_handle_t instance, void **args,
                               const patch_method_signature_t *sig, const bool copy_handles) {
        if (!instance) {
            lua_pushnil(L);
        } else if (copy_handles) {
            push_owned_handle(L, copy_handle(instance));
        } else {
            lua_pushlightuserdata(L, instance);
        }
        const int argc = sig ? static_cast<int>(tefstd_vector_size(&sig->arg_types)) : 0;
        lua_createtable(L, argc, 0);
        for (int i = 0; i < argc; ++i) {
            const auto type = *static_cast<patch_type_t *>(tefstd_vector_at(&sig->arg_types, i));
            push_hook_arg_value(L, type, args[i], copy_handles);
            lua_rawseti(L, -2, i + 1);
        }
    }

    static bool hook_prefix_dispatch(const int slot, patch_handle_t instance, void **args,
                                     const patch_method_signature_t *sig, void *result) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return false;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used || !s.L || s.prefix_ref == LUA_NOREF) return false;

        lua_State *L = s.L;
        const int base = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.prefix_ref);
        push_hook_args(L, instance, args, sig, s.copy_handles);
        if (result && sig && sig->return_type != PATCH_VOID) {
            push_hook_arg_value(L, sig->return_type, result, s.copy_handles);
        } else {
            lua_pushnil(L);
        }

        // 参数：instance, args, result -> 3 个
        if (pcall_log(L, 3, 2, "prefix hook") != LUA_OK) {
            lua_settop(L, base);
            return false;
        }

        const bool skip = lua_toboolean(L, -2) != 0;
        if (skip && result && sig && sig->return_type != PATCH_VOID && !lua_isnil(L, -1)) {
            read_patch_value(L, -1, sig->return_type, result);
        }
        lua_settop(L, base);
        return skip;
    }

    static void hook_postfix_dispatch(const int slot, patch_handle_t instance, void **args, void *result,
                                      const patch_method_signature_t *sig) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used || !s.L || s.postfix_ref == LUA_NOREF) return;

        lua_State *L = s.L;
        const int base = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.postfix_ref);
        push_hook_args(L, instance, args, sig, s.copy_handles);
        if (result && sig && sig->return_type != PATCH_VOID) {
            push_hook_arg_value(L, sig->return_type, result, s.copy_handles);
        } else {
            lua_pushnil(L);
        }

        // 默认忽略 postfix 返回值（兼容旧 Mod）；仅在 opt-in 时用返回值覆盖原方法返回值
        if (s.override_result) {
            if (pcall_log(L, 3, 1, "postfix hook") == LUA_OK) {
                if (result && sig && sig->return_type != PATCH_VOID && !lua_isnil(L, -1)) {
                    read_patch_value(L, -1, sig->return_type, result);
                }
            }
        } else {
            pcall_log(L, 3, 0, "postfix hook");
        }
        lua_settop(L, base);
    }

    static void release_slot(const int slot) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used) return;

        if (s.hook_id != PATCH_HOOK_INVALID_ID) {
            patchlib_uninstall_hook(s.hook_id);
        }
        if (s.L) {
            if (s.prefix_ref != LUA_NOREF) luaL_unref(s.L, LUA_REGISTRYINDEX, s.prefix_ref);
            if (s.postfix_ref != LUA_NOREF) luaL_unref(s.L, LUA_REGISTRYINDEX, s.postfix_ref);
        }
        if (s.handle) {
            auto &slots = s.handle->hook_slots;
            for (auto it = slots.begin(); it != slots.end(); ++it) {
                if (*it == slot) {
                    slots.erase(it);
                    break;
                }
            }
        }
        s = HookSlot{};
    }

    static int alloc_slot() {
        for (int i = 0; i < MAX_HOOK_SLOTS; ++i) {
            if (!g_hook_slots[i].used) {
                g_hook_slots[i].used = true;
                return i;
            }
        }
        return -1;
    }

    static int ref_function_field(lua_State *L, int table_idx, const char *key) {
        if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
        lua_getfield(L, table_idx, key);
        int ref = LUA_NOREF;
        if (lua_isfunction(L, -1)) {
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
        return ref;
    }

    static int l_patch_install_hook(lua_State *L) {
        const patch_handle_t method = to_handle(L, 1);
        if (!method) return luaL_error(L, "invalid method handle");
        luaL_checktype(L, 2, LUA_TTABLE);

        const int slot = alloc_slot();
        if (slot < 0) return luaL_error(L, "no free hook slots (max %d)", MAX_HOOK_SLOTS);

        HookSlot &s = g_hook_slots[slot];
        s.L = L;
        s.handle = get_handle(L);
        s.prefix_ref = ref_function_field(L, 2, "prefix");
        s.postfix_ref = ref_function_field(L, 2, "postfix");
        lua_getfield(L, 2, "copy");
        s.copy_handles = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        // 显式 opt-in 才允许 postfix 返回值覆盖原方法返回值（默认关闭，保证旧 Mod 兼容）
        lua_getfield(L, 2, "override_result");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, 2, "result");
        }
        s.override_result = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        if (s.prefix_ref == LUA_NOREF && s.postfix_ref == LUA_NOREF) {
            s = HookSlot{};
            return luaL_error(L, "hook must define a 'prefix' and/or 'postfix' function");
        }

        const prefix_callback_t prefix = s.prefix_ref != LUA_NOREF ? g_prefix_table[slot] : nullptr;
        const postfix_callback_t postfix = s.postfix_ref != LUA_NOREF ? g_postfix_table[slot] : nullptr;
        s.hook_id = patchlib_install_prepost_hook(method, prefix, postfix);

        if (s.hook_id == PATCH_HOOK_INVALID_ID) {
            if (s.prefix_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.prefix_ref);
            if (s.postfix_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.postfix_ref);
            s = HookSlot{};
            return luaL_error(L, "failed to install hook");
        }

        if (s.handle) s.handle->hook_slots.push_back(slot);

        auto *ud = static_cast<HookUserdata *>(lua_newuserdatauv(L, sizeof(HookUserdata), 0));
        ud->slot = slot;
        luaL_getmetatable(L, HOOK_METATABLE);
        lua_setmetatable(L, -2);
        return 1;
    }

    static int l_hook_remove(lua_State *L) {
        auto *ud = static_cast<HookUserdata *>(luaL_checkudata(L, 1, HOOK_METATABLE));
        if (ud->slot >= 0) {
            release_slot(ud->slot);
            ud->slot = -1;
        }
        return 0;
    }

    static const luaL_Reg mod_patch_functions[] = {
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
            {"string_create", l_patch_string_create},
            {"string_value", l_patch_string_value},
            {"string_empty", l_patch_string_empty},
            {"string_length", l_patch_string_length},
            {"retain", l_patch_retain},
            {"copy", l_patch_retain},
            {"invoke", l_patch_invoke},
            {"install_hook", l_patch_install_hook},
            {"free", l_patch_free},
            {nullptr, nullptr},
    };

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

    static constexpr const char *LUALOADER_PKG_ID = "lzup333.lualoader";

    struct LuaItemFieldValue {
        patch_handle_t field;
        patch_type_t type;
        uint64_t value;
    };

    /// 内核以 terraria_item_handle_t* 回调，故 item 必须是首成员。
    struct LuaItemEntry {
        terraria_item_handle_t item;
        lua_mod_handle_t *owner;
        lua_State *L;
        std::string name;
        std::vector<LuaItemFieldValue> defaults;
        int defaults_ref;
        int can_use_ref;
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
        bool registered;
        int runtime_id;
    };

    static std::vector<LuaItemEntry *> g_lua_items;
    static std::map<int, std::string> g_lua_lang_cache;
    static bool g_lua_item_hooks_registered = false;

    static LuaItemEntry *item_from_handle(terraria_item_handle_t *h) {
        return reinterpret_cast<LuaItemEntry *>(h);
    }

    static void item_field_write(const patch_handle_t field, const patch_handle_t inst,
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
            if (pcall_log(L, 1, 0, "mod.item.defaults") != LUA_OK) lua_pop(L, 1);
        }
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
            lua_pop(L, 1);
            return true;
        }
        const bool r = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return r;
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

    /// 贴图相对路径解析：依次尝试 lib/Image、lib、<Resources>/Image、<Resources>/Resources/Image、<Resources>。
    static std::string resolve_item_image(const std::string &private_dir, const std::string &mod_dir,
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
                                    e->defaults.push_back({f, t, v});
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
        e->item.item_ops.get_texture = lua_item_get_texture;

        if (!terraria_item_manager_register_item(&e->item)) {
            LOG_ERROR("[mod.item] {}: 注册失败", item_name);
            if (e->defaults_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->defaults_ref);
            if (e->can_use_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->can_use_ref);
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
        LOG_INFO("[mod.item] 已注册 {} (runtime_id={})", item_name, e->runtime_id);
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
            if (e->registered && e->item.runtime_id >= 0)
                terraria_item_manager_unregister_item_by_id(e->item.runtime_id);
            if (handle->L) {
                if (e->defaults_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->defaults_ref);
                if (e->can_use_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->can_use_ref);
            }
            delete e;
            g_lua_items.erase(g_lua_items.begin() + static_cast<long>(i));
        }
        rebuild_language_cache();
    }

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
            if (pcall_log(L, 1, 0, "mod.projectile.defaults") != LUA_OK) lua_pop(L, 1);
        }
    }

    static void lua_projectile_ai(terraria_projectile_handle_t *current,
                                  const patch_handle_t inst) {
        LuaProjectileEntry *e = projectile_from_handle(current);
        if (!e || !inst || e->ai_ref == LUA_NOREF || !e->L) return;
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->ai_ref);
        push_handle(L, inst);
        if (pcall_log(L, 1, 0, "mod.projectile.ai") != LUA_OK) lua_pop(L, 1);
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
        e->tex_w = e->tex_h = 0;
        e->tex_tried = false;
        e->registered = false;
        e->runtime_id = -1;

        // no_ai：true = 不接管 AI，沿用原版 aiStyle
        lua_getfield(L, 1, "no_ai");
        const bool no_ai = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

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
                                    e->defaults.push_back({f, t, v});
                                } else {
                                    LOG_WARN("[mod.projectile] {}: 字段 {} 值类型不匹配，已忽略",
                                             proj_name, fname);
                                }
                            } else {
                                LOG_WARN("[mod.projectile] {}: 找不到 Projectile 字段 {}",
                                         proj_name, fname);
                            }
                        }
                        lua_pop(L, 1);
                    }
                }
                patchlib_free(proj_class);
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
        if (no_ai && e->ai_ref != LUA_NOREF)
            LOG_WARN("[mod.projectile] {}: 同时给了 no_ai 和 ai，ai 不会生效", proj_name);

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
        e->projectile.projectile_ops.early_init = nullptr;
        e->projectile.projectile_ops.init_static = nullptr;
        e->projectile.projectile_ops.set_defaults = lua_projectile_set_defaults;
        e->projectile.projectile_ops.ai = (e->ai_ref != LUA_NOREF) ? lua_projectile_ai : nullptr;
        e->projectile.projectile_ops.get_texture = lua_projectile_get_texture;

        if (!terraria_projectile_manager_register_projectile(&e->projectile)) {
            LOG_ERROR("[mod.projectile] {}: 注册失败", proj_name);
            if (e->defaults_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->defaults_ref);
            if (e->ai_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, e->ai_ref);
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
            if (e->registered && e->projectile.runtime_id >= 0)
                terraria_projectile_manager_unregister_projectile_by_id(
                        e->projectile.runtime_id);
            if (handle->L) {
                if (e->defaults_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->defaults_ref);
                if (e->ai_ref != LUA_NOREF)
                    luaL_unref(handle->L, LUA_REGISTRYINDEX, e->ai_ref);
            }
            delete e;
            g_lua_projectiles.erase(g_lua_projectiles.begin() + static_cast<long>(i));
        }
    }

    void register_api(lua_State *L, lua_mod_handle_t *handle) {
        lua_pushlightuserdata(L, handle);
        lua_setfield(L, LUA_REGISTRYINDEX, HANDLE_KEY);

        // 托管句柄元表：__gc 由 Lua GC 自动释放内核句柄（Android 上释放为空操作）
        luaL_newmetatable(L, OWNED_HANDLE_MT);
        lua_pushcfunction(L, l_owned_handle_gc);
        lua_setfield(L, -2, "__gc");
        lua_pop(L, 1);

        // 钩子句柄元表：不再注册 __gc 自动卸载，钩子只在显式 :remove()
        // 或 Mod 卸载时移除（否则丢弃返回值会被 Lua GC 误卸载）
        luaL_newmetatable(L, HOOK_METATABLE);
        lua_pushcfunction(L, l_hook_remove);
        lua_setfield(L, -2, "remove");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pop(L, 1);

        // 全局 mod 表
        lua_newtable(L);

        lua_pushstring(L, handle->mod_id.c_str());
        lua_setfield(L, -2, "id");
        lua_pushstring(L, handle->mod_dir.c_str());
        lua_setfield(L, -2, "mod_dir");
        lua_pushstring(L, handle->private_dir.c_str());
        lua_setfield(L, -2, "private_dir");
        lua_pushstring(L, handle->platform.c_str());
        lua_setfield(L, -2, "platform");
        lua_pushstring(L, handle->arch.c_str());
        lua_setfield(L, -2, "arch");
        lua_pushstring(L, handle->info.version.c_str());
        lua_setfield(L, -2, "version");

        lua_pushcfunction(L, l_mod_log);
        lua_setfield(L, -2, "log");
        lua_pushcfunction(L, l_mod_trace);
        lua_setfield(L, -2, "trace");
        lua_pushcfunction(L, l_mod_debug);
        lua_setfield(L, -2, "debug");
        lua_pushcfunction(L, l_mod_info);
        lua_setfield(L, -2, "info");
        lua_pushcfunction(L, l_mod_warn);
        lua_setfield(L, -2, "warn");
        lua_pushcfunction(L, l_mod_error);
        lua_setfield(L, -2, "error");
        lua_pushcfunction(L, l_mod_fatal);
        lua_setfield(L, -2, "fatal");

        lua_pushcfunction(L, l_mod_read_file);
        lua_setfield(L, -2, "read_file");
        lua_pushcfunction(L, l_mod_write_file);
        lua_setfield(L, -2, "write_file");
        lua_pushcfunction(L, l_mod_file_exists);
        lua_setfield(L, -2, "file_exists");

        lua_newtable(L);
        luaL_setfuncs(L, mod_patch_functions, 0);
        lua_setfield(L, -2, "patch");

        lua_newtable(L);
        luaL_setfuncs(L, mod_item_functions, 0);
        lua_setfield(L, -2, "item");

        lua_newtable(L);
        luaL_setfuncs(L, mod_projectile_functions, 0);
        lua_setfield(L, -2, "projectile");

#ifdef LUALOADER_GUI
        lualoader::gui::register_api(L, handle);
#endif

        lua_pushvalue(L, -1);
        lua_setglobal(L, "mod");
        lua_pop(L, 1);
    }

    void uninstall_all_hooks(lua_mod_handle_t *handle) {
        if (!handle) return;
        const std::vector<int> slots = handle->hook_slots;
        for (const int slot: slots) {
            release_slot(slot);
        }
    }

} // namespace lualoader::lua_api
