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

#include <cstdint>
#include <climits>
#include <memory>
#include <mutex>
#include "json.hpp"
#define STBI_NO_STDIO
#include "vendor/stb_image.h"
#include "tefkernel/terraria/item_manager.h"
#include "tefkernel/terraria/language_manager.h"
#include "tefkernel/terraria/recipe_manager.h"
#include "tefkernel/terraria/texture2d.h"
#include "tefkernel/terraria/asset.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
        patchlib_value_arg_t spec;  // data/data_size/field_types/field_count
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
        sa->spec.data = sa->data;
        sa->spec.data_size = total;
        sa->spec.field_types = sa->field_types;
        sa->spec.field_count = static_cast<size_t>(n);
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
        const int built = build_arg_array(L, arg_start, &sig, storage, argv, MAX_CALL_ARGS);
        if (built < 0) {
            patchlib_method_signature_free(&sig);
            if (built == -1) return luaL_error(L, "too many method arguments (max %d)", MAX_CALL_ARGS);
            return luaL_error(L, "method argument type mismatch");
        }

        // 结构体参数：替换 args[i] 并填 value_args[i]
        patchlib_value_arg_t value_args[MAX_CALL_ARGS] = {};
        for (int i = 0; i < argc && i < MAX_CALL_ARGS; ++i) {
            lua_rawgeti(L, arg_start, i + 1);
            if (auto *sa = static_cast<LuaStructArg *>(luaL_testudata(L, -1, STRUCT_ARG_MT))) {
                argv[i] = sa->data;
                value_args[i] = sa->spec;
            }
            lua_pop(L, 1);
        }

        uint64_t result = 0;
        const bool ok = patchlib_method_invoke_value_args(method, instance, &result, argv, value_args);
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

    struct ItemBinding {
        terraria_item_handle_t item{};
        lua_mod_handle_t *owner{};
        std::string mod_id, name, display_name, tooltip, texture_path;
        int refs[5]{LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};
        patch_handle_t texture{};
        int recipe_stack{0}, station{-1};
        struct Material { int id, stack; std::string name; };
        std::vector<Material> materials;
    };
    static std::vector<std::unique_ptr<ItemBinding>> item_bindings;
    static std::mutex item_text_mutex;
    static std::string item_text_json = "{}";
    static bool item_callbacks_registered = false;
    static const char *item_callback_names[] = {
            "early_init", "init_static", "set_defaults", "can_use", "get_texture"};

    static bool items_available() {
        return terraria_item_manager_get_api_version &&
               terraria_item_manager_get_api_version() >= 1 &&
               terraria_item_manager_register_item && terraria_item_manager_unregister_item &&
               terraria_language_manager_register_callback &&
               terraria_recipe_manager_register_callback;
    }

    static ItemBinding *find_item(lua_mod_handle_t *owner, const char *name) {
        for (auto &b : item_bindings)
            if (b->owner == owner && b->name == name) return b.get();
        return nullptr;
    }

    static ItemBinding *binding_for(terraria_item_handle_t *item) {
        for (auto &b : item_bindings)
            if (&b->item == item) return b.get();
        return nullptr;
    }

    static void rebuild_item_text() {
        nlohmann::json text = nlohmann::json::object();
        for (const auto &b : item_bindings) {
            if (!b->owner) continue;
            const std::string suffix = "lzup333.lualoader_" + b->mod_id;
            text["item_name_" + suffix][b->name] = b->display_name;
            if (!b->tooltip.empty()) text["item_tooltip_" + suffix][b->name] = b->tooltip;
        }
        std::lock_guard<std::mutex> lock(item_text_mutex);
        item_text_json = text.dump();
    }

    static const char *item_language_callback(language_culture_t) {
        thread_local std::string snapshot;
        std::lock_guard<std::mutex> lock(item_text_mutex);
        snapshot = item_text_json;
        return snapshot.c_str();
    }

    static void item_recipe_callback() {
        if (!terraria_recipe_set_result || !terraria_recipe_set_material ||
            !terraria_recipe_set_station || !terraria_recipe_add) return;
        for (auto &b : item_bindings) {
            if (!b->owner || b->recipe_stack <= 0 || b->item.runtime_id < 0) continue;
            std::vector<int> mats;
            bool valid = true;
            for (auto &m : b->materials) {
                ItemBinding *custom = m.name.empty() ? nullptr : find_item(b->owner, m.name.c_str());
                int id = m.name.empty() ? m.id : (custom ? custom->item.runtime_id : -1);
                if (id <= 0) { valid = false; break; }
                mats.push_back(id);
                mats.push_back(m.stack);
            }
            if (!valid) {
                LOG_ERROR("[mod {}] unresolved recipe material for {}", b->mod_id, b->name);
                continue;
            }
            terraria_recipe_set_result(b->item.runtime_id, b->recipe_stack);
            terraria_recipe_set_material(mats.data(), static_cast<int>(mats.size() / 2));
            terraria_recipe_set_station(b->station);
            terraria_recipe_add();
        }
    }

    static bool begin_item_call(ItemBinding *b, int slot, int &top) {
        if (!b || !b->owner || !b->owner->L || b->refs[slot] == LUA_NOREF) return false;
        lua_State *L = b->owner->L;
        top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, b->refs[slot]);
        return true;
    }

    static bool finish_item_call(ItemBinding *b, int slot, int top, int nargs, int results) {
        lua_State *L = b->owner->L;
        if (lua_pcall(L, nargs, results, 0) == LUA_OK) return true;
        LOG_ERROR("[mod {}] item {}.{}: {}", b->mod_id, b->name,
                  item_callback_names[slot], lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua error");
        lua_settop(L, top);
        // A failing callback is disabled until reload to avoid per-frame log spam.
        luaL_unref(L, LUA_REGISTRYINDEX, b->refs[slot]);
        b->refs[slot] = LUA_NOREF;
        return false;
    }

    static void item_init_callback(terraria_item_handle_t *item, int slot) {
        auto *b = binding_for(item);
        int top;
        if (!begin_item_call(b, slot, top)) return;
        lua_State *L = b->owner->L;
        lua_pushinteger(L, item->runtime_id);
        finish_item_call(b, slot, top, 1, 0);
        lua_settop(L, top);
    }
    static void item_early_init(terraria_item_handle_t *item) { item_init_callback(item, 0); }
    static void item_init_static(terraria_item_handle_t *item) { item_init_callback(item, 1); }
    static void item_set_defaults(terraria_item_handle_t *item, patch_handle_t instance) {
        auto *b = binding_for(item);
        int top;
        if (!begin_item_call(b, 2, top)) return;
        lua_State *L = b->owner->L;
        push_owned_handle(L, copy_handle(instance));
        lua_pushinteger(L, item->runtime_id);
        finish_item_call(b, 2, top, 2, 0);
        lua_settop(L, top);
    }
    static bool item_can_use(terraria_item_handle_t *item, patch_handle_t player,
                             patch_handle_t instance, bool ignore_cursed) {
        auto *b = binding_for(item);
        int top;
        if (!begin_item_call(b, 3, top)) return true;
        lua_State *L = b->owner->L;
        push_owned_handle(L, copy_handle(player));
        push_owned_handle(L, copy_handle(instance));
        lua_pushboolean(L, ignore_cursed);
        lua_pushinteger(L, item->runtime_id);
        bool allowed = false;
        if (finish_item_call(b, 3, top, 4, 1)) allowed = lua_toboolean(L, -1);
        lua_settop(L, top);
        return allowed;
    }

    static patch_handle_t item_get_texture(terraria_item_handle_t *item) {
        auto *b = binding_for(item);
        if (!b || !b->owner) return nullptr;
        if (b->texture) return copy_handle(b->texture);
        int top;
        if (begin_item_call(b, 4, top)) {
            lua_State *L = b->owner->L;
            lua_pushinteger(L, item->runtime_id);
            if (finish_item_call(b, 4, top, 1, 1)) {
                patch_handle_t h = to_handle(L, -1);
                if (h) b->texture = copy_handle(h);
            }
            lua_settop(L, top);
        } else if (!b->texture_path.empty() && terraria_texture2d_create &&
                   terraria_texture2d_get_class && terraria_asset_create) {
            std::ifstream input(b->texture_path, std::ios::binary | std::ios::ate);
            const auto size = input ? input.tellg() : std::streampos(-1);
            if (size <= 0 || size > 16 * 1024 * 1024) return nullptr;
            std::vector<unsigned char> data(static_cast<size_t>(size));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char *>(data.data()), data.size())) return nullptr;
            int w, h, channels;
            if (!stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &channels) ||
                w <= 0 || h <= 0 || w > 2048 || h > 2048) return nullptr;
            auto *pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &channels, 4);
            if (!pixels) return nullptr;
#if defined(__ANDROID__)
            for (int y = 0; y < h / 2; ++y)
                for (int x = 0; x < w * 4; ++x)
                    std::swap(pixels[y * w * 4 + x], pixels[(h - 1 - y) * w * 4 + x]);
#endif
            patch_handle_t texture = terraria_texture2d_create(w, h, TEXTURE_FORMAT_RGBA32,
                                                               pixels, static_cast<size_t>(w) * h * 4);
            stbi_image_free(pixels);
            if (texture) {
                patch_handle_t type = terraria_texture2d_get_class();
                b->texture = terraria_asset_create(type, texture);
                patchlib_free(type);
                patchlib_free(texture);
            }
            if (!b->texture) LOG_ERROR("[mod {}] cannot load item texture {}", b->mod_id, b->name);
        }
        return b->texture ? copy_handle(b->texture) : nullptr;
    }

    static void release_item_binding(ItemBinding &b) {
        if (b.owner && b.owner->L) {
            for (int &ref : b.refs) {
                luaL_unref(b.owner->L, LUA_REGISTRYINDEX, ref);
                ref = LUA_NOREF;
            }
        }
        if (b.texture) patchlib_free(b.texture);
        b.texture = nullptr;
    }

    void unregister_all_items(lua_mod_handle_t *owner) {
        for (auto it = item_bindings.begin(); it != item_bindings.end();) {
            auto &b = **it;
            if (b.owner != owner) { ++it; continue; }
            const bool detached = terraria_item_manager_unregister_item &&
                                  terraria_item_manager_unregister_item(&b.item);
            release_item_binding(b);
            if (detached) it = item_bindings.erase(it);
            else {
                // Allocation failure in kernel: keep the native handle alive, but detach Lua.
                b.owner = nullptr;
                ++it;
            }
        }
        rebuild_item_text();
    }

    static int l_items_available(lua_State *L) {
        lua_pushboolean(L, items_available());
        return 1;
    }
    static int l_items_get_id(lua_State *L) {
        const char *name = luaL_checkstring(L, 1);
        auto *b = find_item(get_handle(L), name);
        if (!b || b->item.runtime_id < 0) lua_pushnil(L);
        else lua_pushinteger(L, b->item.runtime_id);
        return 1;
    }
    static int l_items_unregister(lua_State *L) {
        auto *b = find_item(get_handle(L), luaL_checkstring(L, 1));
        if (!b) { lua_pushboolean(L, false); return 1; }
        // Registration/unregistration during item callbacks would invalidate their context.
        if (b->item.runtime_id >= 0) return luaL_error(L, "unregister items by unloading the mod after initialization");
        if (!terraria_item_manager_unregister_item(&b->item)) return luaL_error(L, "kernel could not unregister item");
        release_item_binding(*b);
        for (auto it = item_bindings.begin(); it != item_bindings.end(); ++it)
            if (it->get() == b) { item_bindings.erase(it); break; }
        rebuild_item_text();
        lua_pushboolean(L, true);
        return 1;
    }

    static int l_items_register(lua_State *L) {
        if (!items_available()) return luaL_error(L, "custom items require the matching TEFKernel item API v1 build");
        luaL_checktype(L, 1, LUA_TTABLE);
        auto *owner = get_handle(L);
        lua_getfield(L, 1, "internal_name");
        const char *name = luaL_checkstring(L, -1);
        if (!*name || strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != strlen(name))
            return luaL_error(L, "internal_name must contain only letters, digits or underscores");
        if (find_item(owner, name)) return luaL_error(L, "item already registered: %s", name);
        // Validate before allocating C++ state: Lua errors use longjmp.
        for (auto key : {"display_name", "tooltip", "texture"}) {
            lua_getfield(L, 1, key);
            if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TSTRING)
                return luaL_error(L, "%s must be a string", key);
            lua_pop(L, 1);
        }
        for (const char *key : item_callback_names) {
            lua_getfield(L, 1, key);
            if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
                return luaL_error(L, "%s must be a function", key);
            lua_pop(L, 1);
        }
        lua_getfield(L, 1, "sacrifice_count");
        const lua_Integer sacrifice = luaL_optinteger(L, -1, 0);
        if (sacrifice < 0 || sacrifice > 9999) return luaL_error(L, "invalid sacrifice_count");
        lua_pop(L, 1);
        lua_getfield(L, 1, "recipe");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            for (auto key : {"stack", "station"}) {
                lua_getfield(L, -1, key);
                const auto value = luaL_optinteger(L, -1, strcmp(key, "stack") == 0 ? 1 : -1);
                if (value < (strcmp(key, "stack") == 0 ? 1 : -1) || value > INT_MAX)
                    return luaL_error(L, "invalid recipe %s", key);
                lua_pop(L, 1);
            }
            lua_getfield(L, -1, "materials");
            luaL_checktype(L, -1, LUA_TTABLE);
            const size_t count = lua_rawlen(L, -1);
            if (count < 1 || count > 15) return luaL_error(L, "recipe needs 1 to 15 materials");
            for (size_t i = 1; i <= count; ++i) {
                lua_rawgeti(L, -1, i);
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_rawgeti(L, -1, 1);
                if (lua_type(L, -1) != LUA_TSTRING && (!lua_isinteger(L, -1) || lua_tointeger(L, -1) <= 0 || lua_tointeger(L, -1) > INT_MAX))
                    return luaL_error(L, "material ID must be positive or an internal_name string");
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 2);
                const auto stack = luaL_checkinteger(L, -1);
                if (stack < 1 || stack > 9999) return luaL_error(L, "invalid material stack");
                lua_pop(L, 2);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        auto b = std::make_unique<ItemBinding>();
        b->owner = owner;
        b->mod_id = owner->info.pkg_id;
        b->name = name;
        auto read_string = [&](const char *key) {
            lua_getfield(L, 1, key);
            std::string value = lua_isnil(L, -1) ? "" : lua_tostring(L, -1);
            lua_pop(L, 1);
            return value;
        };
        b->display_name = read_string("display_name");
        if (b->display_name.empty()) b->display_name = b->name;
        b->tooltip = read_string("tooltip");
        b->texture_path = read_string("texture");
        if (!b->texture_path.empty()) {
            const std::filesystem::path path(b->texture_path);
            // Texture paths are resolved relative to the same directory as the Lua entry.
            b->texture_path = (path.is_absolute() ? path : std::filesystem::path(owner->mod_dir) / path).string();
        }
        for (int i = 0; i < 5; ++i) {
            lua_getfield(L, 1, item_callback_names[i]);
            if (lua_isnil(L, -1)) lua_pop(L, 1);
            else b->refs[i] = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        lua_getfield(L, 1, "recipe");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "stack"); b->recipe_stack = static_cast<int>(lua_tointeger(L, -1)); lua_pop(L, 1);
            if (!b->recipe_stack) b->recipe_stack = 1;
            lua_getfield(L, -1, "station"); b->station = lua_isnil(L, -1) ? -1 : static_cast<int>(lua_tointeger(L, -1)); lua_pop(L, 1);
            lua_getfield(L, -1, "materials");
            for (size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
                lua_rawgeti(L, -1, i);
                lua_rawgeti(L, -1, 1);
                ItemBinding::Material m{};
                if (lua_type(L, -1) == LUA_TSTRING) m.name = lua_tostring(L, -1);
                else m.id = static_cast<int>(lua_tointeger(L, -1));
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 2); m.stack = static_cast<int>(lua_tointeger(L, -1)); lua_pop(L, 2);
                b->materials.push_back(std::move(m));
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        b->item.parent_modloader_id = "lzup333.lualoader";
        b->item.parent_id = b->mod_id.c_str();
        b->item.internal_name = b->name.c_str();
        b->item.runtime_id = -1;
        b->item.has_tooltip = !b->tooltip.empty();
        b->item.sacrifice_count = static_cast<int>(sacrifice);
        b->item.item_ops = {item_early_init, item_init_static, item_set_defaults, item_can_use, item_get_texture};
        if (!terraria_item_manager_register_item(&b->item)) {
            release_item_binding(*b);
            b.reset();
            return luaL_error(L, "kernel rejected item; register new items before Main.Initialize");
        }
        item_bindings.push_back(std::move(b));
        if (!item_callbacks_registered) {
            terraria_language_manager_register_callback(item_language_callback);
            terraria_recipe_manager_register_callback(item_recipe_callback);
            item_callbacks_registered = true;
        }
        rebuild_item_text();
        lua_pushstring(L, name);
        return 1;
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

        lua_newtable(L);
        static const luaL_Reg item_functions[] = {
            {"available", l_items_available}, {"register", l_items_register},
            {"get_id", l_items_get_id}, {"unregister", l_items_unregister}, {nullptr, nullptr}
        };
        luaL_setfuncs(L, item_functions, 0);
        lua_setfield(L, -2, "items");

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
