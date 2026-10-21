/*******************************************************************************
 * LuaLoader - core
 * Copyright (C) 2026 lzup333
 * Based on KernelLoader (https://github.com/eternalfuture-e38299/TEFKernel-KernelLoader)
 *   Copyright (C) 2026 eternalfuture-e38299
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

#include "core.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "json.hpp"
#include "logger.hpp"
#include "tefkernel/terraria/netmanager.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/terraria/main.h"
#include "lua_engine.hpp"
#include "platform.hpp"
#ifdef LUALOADER_GUI
#include "gui.hpp"
#endif

#include <cstdint>
#include <set>
#include <vector>

#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/string.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/tefstd/vector.h"

API_EXPORT const ml_ops_t *API_CALL ml_create() {
    static ml_ops_t ml_ops{};

    ml_ops.cleanup_ml = lualoader::core::cleanup_ml;
    ml_ops.get_info = lualoader::core::get_info;
    ml_ops.get_multiplayer_info = lualoader::core::get_multiplayer_info;
    ml_ops.init_ml = lualoader::core::init_ml;
    ml_ops.init_mod = lualoader::core::init_mod;
    ml_ops.load_mod = lualoader::core::load_mod;
    ml_ops.reload_mod = nullptr; /* 热重载已移除 */
    ml_ops.unload_mod = lualoader::core::unload_mod;

    return &ml_ops;
}



ml_result_t lualoader::core::load_mod(mod_manifest_t *mod_manifest) {
    if (!mod_manifest || !mod_manifest->path) {
        LOG_ERROR("load_mod: null manifest or path");
        return ML_ERROR_INVALID_PARAM;
    }

    // 重复加载同一个 Mod 会被忽略（热重载已移除；更换版本请重启游戏）。
    const std::string mod_id = mod_manifest->mod_id ? mod_manifest->mod_id : "";
    if (lua_mod_handles.find(mod_id) != lua_mod_handles.end()) {
        LOG_WARN("Lua mod already loaded: {}", mod_id);
        return ML_SUCCESS;
    }

    LOG_INFO("Starting to load Lua mod: mod_id={}, path={}, private_dir={}",
             mod_manifest->mod_id ? mod_manifest->mod_id : "(null)",
             mod_manifest->path,
             mod_manifest->private_dir ? mod_manifest->private_dir : "(null)");

    std::filesystem::path config_path(mod_manifest->path);
    if (!std::filesystem::exists(config_path)) {
        LOG_ERROR("Mod config not found: {}", config_path.string());
        return ML_ERROR_NOT_FOUND;
    }

    auto *handle = new lua_mod_handle_t;
    auto *multiplayer_info = new multiplayer_mod_info_t;

    try {
        std::ifstream mod_file(config_path);
        auto json_data = nlohmann::json::parse(mod_file);

        handle->mod_id = mod_manifest->mod_id ? mod_manifest->mod_id : "";
        handle->config_path = config_path.string();
        handle->logs_dir = mod_manifest->logs_dir ? mod_manifest->logs_dir : "";
        handle->entry = json_data.value("main", std::string("main.lua"));
        handle->info.pkg_id = json_data.value("pkg_id", json_data.value("name", handle->mod_id));
        handle->info.version = json_data.value("version", std::string("1.0.0"));
        handle->info.version_code = json_data.value("version_code", 1);
        handle->info.api_version = json_data.value("api_version", 1);
        const bool multiplayer_safe = json_data.value("multiplayer_safe", true);

        std::string err;
        const std::string private_dir = mod_manifest->private_dir
                ? mod_manifest->private_dir : config_path.parent_path().string();
        if (!lua_engine::load(handle, config_path.string(), private_dir, err)) {
            LOG_ERROR("Failed to load Lua mod {}: {}", handle->mod_id, err);
            // load 失败时可能已预加载原生库，必须 close 以释放（否则句柄泄漏/残留）
            lua_engine::close(handle);
            delete handle;
            delete multiplayer_info;
            return ML_ERROR;
        }

        multiplayer_info->mod_id = strdup(handle->info.pkg_id.c_str());
        multiplayer_info->is_multiplayer_safe = multiplayer_safe ? 1 : 0;
        multiplayer_info->version = strdup(handle->info.version.c_str());
        multiplayer_info->version_code = handle->info.version_code;

        lua_mod_handles.emplace(handle->mod_id, handle);
        lua_mod_multiplayer_infos.emplace(handle->mod_id, multiplayer_info);

        LOG_INFO("Lua mod loaded: {} v{} ({}), entry={}",
                 handle->info.pkg_id, handle->info.version, handle->mod_id, handle->entry);
        return ML_SUCCESS;
    } catch (const nlohmann::json::parse_error &e) {
        LOG_ERROR("JSON parse error while loading mod {}: {}", mod_manifest->mod_id, e.what());
    } catch (const std::filesystem::filesystem_error &e) {
        LOG_ERROR("Filesystem error while loading mod {}: {}", mod_manifest->mod_id, e.what());
    } catch (const std::exception &e) {
        LOG_ERROR("Exception while loading mod {}: {}", mod_manifest->mod_id, e.what());
    }

    lua_engine::close(handle);
    delete handle;
    delete multiplayer_info;
    return ML_ERROR;
}

ml_result_t lualoader::core::unload_mod(mod_manifest_t *mod_manifest) {
    if (!mod_manifest || !mod_manifest->mod_id) {
        LOG_ERROR("unload_mod: null manifest or mod_id");
        return ML_ERROR_INVALID_PARAM;
    }
    LOG_INFO("Unloading Lua mod: {}", mod_manifest->mod_id);

    const auto it = lua_mod_handles.find(mod_manifest->mod_id);
    if (it == lua_mod_handles.end()) {
        LOG_WARN("Mod not found: {}", mod_manifest->mod_id);
        return ML_ERROR_NOT_FOUND;
    }

    auto *handle = it->second;

    LOG_DEBUG("Calling mod.cleanup ...");
    lua_engine::call_cleanup(handle);
    lua_engine::close(handle);
    delete handle;

    const auto info_it = lua_mod_multiplayer_infos.find(mod_manifest->mod_id);
    if (info_it != lua_mod_multiplayer_infos.end()) {
        free(const_cast<char *>(info_it->second->mod_id));
        free(const_cast<char *>(info_it->second->version));
        delete info_it->second;
        lua_mod_multiplayer_infos.erase(info_it);
    }

    lua_mod_handles.erase(it);
    LOG_INFO("Lua mod unloaded: {}", mod_manifest->mod_id);
    return ML_SUCCESS;
}

void lualoader::core::call_all_on_gui() {
    for (auto &kv: lua_mod_handles) {
        lua_engine::call_on_gui(kv.second);
    }
}

ml_result_t lualoader::core::init_mod(mod_manifest_t *mod_manifest) {
    if (!mod_manifest || !mod_manifest->mod_id) return ML_ERROR_INVALID_PARAM;
    LOG_INFO("Initializing Lua mod: {}", mod_manifest->mod_id);

    const auto it = lua_mod_handles.find(mod_manifest->mod_id);
    if (it == lua_mod_handles.end()) {
        LOG_ERROR("Cannot initialize mod - not found: {}", mod_manifest->mod_id);
        return ML_ERROR_NOT_FOUND;
    }

    lua_engine::call_init(it->second);
    LOG_INFO("Lua mod initialized: {}", mod_manifest->mod_id);
    return ML_SUCCESS;
}

const multiplayer_mod_info_t *lualoader::core::get_multiplayer_info(mod_manifest_t *mod_manifest) {
    if (!mod_manifest || !mod_manifest->mod_id) return nullptr;
    const auto it = lua_mod_multiplayer_infos.find(mod_manifest->mod_id);
    if (it == lua_mod_multiplayer_infos.end()) {
        LOG_WARN("Multiplayer info not found for mod: {}", mod_manifest->mod_id);
        return nullptr;
    }
    return it->second;
}

namespace {
    bool g_ml_inited = false; // init_ml 幂等（内核可能调用两次）
} // namespace

/// 内核收到的 Mod 消息 → 对应 Mod 的 mod.net.on_receive(channel, payload, player)
static void on_mod_message(const char *mod_id, const char *channel,
                           const uint8_t *data, int len, int player) {
    if (!mod_id) return;
    const auto it = lualoader::core::lua_mod_handles.find(mod_id);
    if (it == lualoader::core::lua_mod_handles.end() || !it->second) return;
    lualoader::lua_mod_handle_t *h = it->second;
    if (!h->L || h->net_recv_ref == LUA_NOREF) return;

    lua_State *L = h->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->net_recv_ref);
    lua_pushstring(L, channel ? channel : "");
    lua_pushlstring(L, reinterpret_cast<const char *>(data), (size_t) len);
    lua_pushinteger(L, player);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        LOG_ERROR("[net] {}: on_receive failed: {}", mod_id, err ? err : "?");
        lua_pop(L, 1);
    }
}

ml_result_t lualoader::core::init_ml(ml_entry_t *ml_entry) {
    LOG_INFO("Initializing LuaLoader...");
    LOG_DEBUG("Platform: {}, Architecture: {}", LUALOADER_PLATFORM_NAME, LUALOADER_ARCH_NAME);

    if (g_ml_inited) {
        LOG_DEBUG("[init] init_ml called again, ignored");
        return ML_SUCCESS;
    }
    g_ml_inited = true;

    logger::init();
    logger::init_mod();

    // 联机：注册内核 Mod 消息回调（客户端发包 → 服务端这里分发）
    terraria_net_set_mod_message_handler(on_mod_message);

    // 读取加载器隐藏配置（<private_dir>/config.json），按需开启调试工具
    if (ml_entry && ml_entry->private_dir) {
#ifdef LUALOADER_GUI
        gui::configure_storage(ml_entry->private_dir);
#endif

    }

#ifdef LUALOADER_GUI
    // 原生 GUI 对 Mod 永久开放：任何定义了 mod.on_gui 的 Mod 都会被绘制。
    // 绘制走游戏自身的 XNA/Unity SpriteBatch。
    // 服务端（TerrariaServer）没有 GraphicsDevice / spriteBatch，
    // 安装 GUI 钩子会空指针崩溃，必须跳过。
    if (terraria_main_is_server())
        LOG_INFO("[gui] server mode: native GUI disabled");
    else
        gui::install();
#endif

    LOG_INFO("LuaLoader initialized successfully");
    return ML_SUCCESS;
}

ml_result_t lualoader::core::cleanup_ml(ml_entry_t *ml_entry) {
    LOG_INFO("Cleaning up LuaLoader...");


#ifdef LUALOADER_GUI
    gui::shutdown();
#endif

    for (auto &entry: lua_mod_handles) {
        lua_engine::call_cleanup(entry.second);
        lua_engine::close(entry.second);
        delete entry.second;
    }
    lua_mod_handles.clear();

    for (auto &entry: lua_mod_multiplayer_infos) {
        free(const_cast<char *>(entry.second->mod_id));
        free(const_cast<char *>(entry.second->version));
        delete entry.second;
    }
    lua_mod_multiplayer_infos.clear();

    LOG_INFO("LuaLoader cleaned up successfully");

    logger::shutdown_mod();
    logger::shutdown();

    g_ml_inited = false;
    return ML_SUCCESS;
}

const ml_info_t *lualoader::core::get_info() {
    static ml_info_t info = {
            "lzup333.lualoader",
            32,
            "2.1.0-dev",
            1,
            0,
            nullptr,
    };
    return &info;
}
