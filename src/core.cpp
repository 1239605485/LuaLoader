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
    ml_ops.reload_mod = lualoader::core::reload_mod;
    ml_ops.unload_mod = lualoader::core::unload_mod;

    return &ml_ops;
}

namespace {
    std::string g_mods_root;  // <workdir>/mods/<loader_pkgid>，热加载扫描用
    void show_chat(const char *text);
}

ml_result_t lualoader::core::load_mod(mod_manifest_t *mod_manifest) {
    if (!mod_manifest || !mod_manifest->path) {
        LOG_ERROR("load_mod: null manifest or path");
        return ML_ERROR_INVALID_PARAM;
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
        if (!lua_engine::load(handle, config_path.string(), mod_manifest->private_dir, err)) {
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

ml_result_t lualoader::core::reload_mod(mod_manifest_t *mod_manifest) {
    if (!mod_manifest) return ML_ERROR_INVALID_PARAM;
    // 内核（或调试工具）可能用空 manifest 触发“重载全部”
    if (!mod_manifest->mod_id) {
        reload_all_mods();
        return ML_SUCCESS;
    }

    LOG_INFO("Reloading Lua mod: {}", mod_manifest->mod_id);
    if (lua_mod_handles.find(mod_manifest->mod_id) != lua_mod_handles.end()) {
        unload_mod(mod_manifest);
    }
    const ml_result_t r = load_mod(mod_manifest);
    if (r == ML_SUCCESS) init_mod(mod_manifest); // 重载后需重跑 setup / 重新装钩子
    return r;
}

void lualoader::core::reload_all_mods() {
    namespace fs = std::filesystem;
    const auto strip = [](const std::string &s) {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
        return s.substr(a, b - a);
    };

    // mods 根目录：优先 init_ml 推导，否则从已加载 Mod 反推
    std::string mods_root = g_mods_root;
    if (mods_root.empty() && !lua_mod_handles.empty()) {
        mods_root = fs::path(lua_mod_handles.begin()->second->config_path).parent_path().parent_path().string();
    }

    // 读 enables.txt；不存在则退化为“只重载已加载的”
    std::set<std::string> enabled;
    const bool have_enables = !mods_root.empty() && fs::exists(fs::path(mods_root) / "enables.txt");
    if (have_enables) {
        std::ifstream in(fs::path(mods_root) / "enables.txt");
        std::string line;
        while (std::getline(in, line)) {
            const std::string id = strip(line);
            if (!id.empty() && id[0] != '#') enabled.insert(id);
        }
    }

    // 1) 已加载但不再启用 → 卸载
    if (have_enables) {
        std::vector<std::string> loaded;
        loaded.reserve(lua_mod_handles.size());
        for (const auto &kv: lua_mod_handles) loaded.push_back(kv.first);
        for (const auto &id: loaded) {
            if (enabled.count(id)) continue;
            mod_manifest_t m{};
            m.mod_id = id.c_str();
            unload_mod(&m);
        }
    }

    // 2) 目标列表：启用列表，或（无 enables.txt 时）当前已加载
    std::vector<std::string> targets;
    if (have_enables) targets.assign(enabled.begin(), enabled.end());
    else for (const auto &kv: lua_mod_handles) targets.push_back(kv.first);

    size_t n = 0;
    for (const auto &id: targets) {
        std::string cfg, priv, logs;
        if (have_enables) {
            cfg = (fs::path(mods_root) / "mod" / id).string();
            if (!fs::exists(cfg)) continue;
            priv = (fs::path(mods_root) / "private" / id).string();
            logs = (fs::path(mods_root) / "logs" / id).string();
        } else {
            const auto it = lua_mod_handles.find(id);
            if (it == lua_mod_handles.end()) continue;
            cfg = it->second->config_path;
            priv = it->second->private_dir;
            logs = it->second->logs_dir;
        }
        mod_manifest_t m{};
        m.path = cfg.c_str();
        m.mod_id = id.c_str();
        m.private_dir = priv.c_str();
        m.logs_dir = logs.c_str();

        if (lua_mod_handles.find(id) != lua_mod_handles.end()) {
            reload_mod(&m);
        } else {
            LOG_INFO("[reload] hot-loading new mod: {}", id);
            if (load_mod(&m) == ML_SUCCESS) init_mod(&m);
        }
        ++n;
    }

    LOG_INFO("[reload] done ({} mod(s))", n);
    const std::string msg = "LuaLoader: reloaded " + std::to_string(n) + " mod(s)";
    show_chat(msg.c_str());
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

// ============================================================================
// 调试工具：隐藏配置 + /reload（仅 debug 模式启用）
//   配置位置：<loader private_dir>/config.json  （普通玩家不会接触到）
//   debug 打开后：游戏内聊天输入 /reload 即可重载 LuaLoader 的所有 Mod
// ============================================================================

namespace {
    bool g_debug = false;
    bool g_ml_inited = false; // init_ml 幂等（内核可能调用两次）
    volatile bool g_pending_reload = false;
    patch_hook_id_t g_hook_chat = PATCH_HOOK_INVALID_ID;
    patch_hook_id_t g_hook_send = PATCH_HOOK_INVALID_ID;
    patch_hook_id_t g_hook_frame = PATCH_HOOK_INVALID_ID;
    patch_handle_t g_get_msg_text = nullptr;  // ChatMessage.Text getter
    patch_handle_t g_my_player_field = nullptr; // Main.myPlayer
    patch_handle_t g_new_text_method = nullptr; // Main.NewText

    const char *trim_left(const char *s) {
        if (!s) return "";
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
        return s;
    }

    /// 读取 ChatMessage.Text（优先 getter，Android 回退对象偏移 0x18）
    void *chat_message_text(void *message) {
        if (!message) return nullptr;
        if (g_get_msg_text) {
            void *str = nullptr;
            if (patchlib_method_invoke_args(g_get_msg_text, message, &str, nullptr) && str) {
                return str;
            }
        }
#if defined(__ANDROID__)
        return *(void **) ((char *) message + 0x18);
#else
        return nullptr;
#endif
    }

    /// 在聊天框显示一行提示（尽量而为，失败就算了）
    void show_chat(const char *text) {
        if (!text || !g_new_text_method) return;
        void *s = patchlib_string_create(text);
        if (!s) return;
        uint8_t r = 255, g = 255, b = 255;
        bool broadcast = false;
        void *args[5] = {&s, &r, &g, &b, &broadcast};
        patchlib_method_invoke_args(g_new_text_method, nullptr, nullptr, args);
        patchlib_free(s);
    }

    void handle_chat_message(void *message) {
        void *str = chat_message_text(message);
        if (!str) return;
        char *text = patchlib_string_cstr(str);
        if (!text) return;
        if (std::strcmp(trim_left(text), "/reload") == 0) {
            g_pending_reload = true;
        }
        free(text);
    }

    void chat_postfix(patch_handle_t, void **args, void *, const patch_method_signature_t *) {
        if (!args || !args[0]) return;
        handle_chat_message(*(void **) args[0]);
    }

    /// 每帧 tick：把 pending 的重载放到这里执行（脱离 hook 链，安全）
    void frame_postfix(patch_handle_t, void **, void *, const patch_method_signature_t *) {
        if (!g_pending_reload) return;
        g_pending_reload = false;
        LOG_INFO("[debug] /reload triggered, reloading all LuaLoader mods");
        lualoader::core::reload_all_mods();
    }

    void load_config(const std::string &private_dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(private_dir, ec);
        const fs::path cfg = fs::path(private_dir) / "config.json";
        if (!fs::exists(cfg)) {
            std::ofstream out(cfg);
            out << "{\n  \"debug\": false\n}\n";
            LOG_INFO("[debug] created loader config: {}", cfg.string());
        }
        try {
            std::ifstream in(cfg);
            const auto j = nlohmann::json::parse(in);
            g_debug = j.value("debug", false);
        } catch (const std::exception &e) {
            LOG_WARN("[debug] failed to parse config ({}), debug disabled", e.what());
            g_debug = false;
        }
        LOG_INFO("[debug] debug mode = {}", g_debug);
    }

    void install_debug_hooks() {
        const patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");

        // 每帧 tick：优先跨平台目标 Terraria.Main.Update(GameTime)（PC/PE 都有），
        // 退回 Main.DoUpdate / Player.ResetEffects / PE 的 XNAUnityRunner.Update
        patch_handle_t frame_method = nullptr;
        if (main_type) {
            frame_method = patchlib_type_get_method_by_param_count(main_type, "Update", 1);
            if (!frame_method) frame_method = patchlib_type_get_method_by_param_count(main_type, "DoUpdate", 1);
        }
        if (!frame_method) {
            const patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
            if (player_type) frame_method = patchlib_type_get_method_by_param_count(player_type, "ResetEffects", 0);
        }
        if (!frame_method) {
            const patch_handle_t ut = patchlib_type_get_type("", "XNAUnityRunner");
            if (ut) frame_method = patchlib_type_get_method_by_param_count(ut, "Update", 0);
        }
        if (frame_method) g_hook_frame = patchlib_install_prepost_hook(frame_method, nullptr, frame_postfix);

        // 聊天指令（单机 ProcessIncomingMessage / 联机 SendChatMessageFromClient）
        const patch_handle_t ct = patchlib_type_get_type("Terraria.Chat", "ChatCommandProcessor");
        const patch_handle_t cm = ct ? patchlib_type_get_method_by_param_count(ct, "ProcessIncomingMessage", 2) : nullptr;
        if (cm) g_hook_chat = patchlib_install_prepost_hook(cm, nullptr, chat_postfix);

        const patch_handle_t ht = patchlib_type_get_type("Terraria.Chat", "ChatHelper");
        const patch_handle_t sm = ht ? patchlib_type_get_method_by_param_count(ht, "SendChatMessageFromClient", 1) : nullptr;
        if (sm) g_hook_send = patchlib_install_prepost_hook(sm, nullptr, chat_postfix);

        // 辅助句柄
        const patch_handle_t msg_type = patchlib_type_get_type("Terraria.Chat", "ChatMessage");
        if (msg_type) {
            const patch_handle_t text_prop = patchlib_type_get_property(msg_type, "Text");
            if (text_prop) g_get_msg_text = patchlib_property_get_get_method(text_prop);
        }
        if (main_type) {
            g_my_player_field = patchlib_type_get_field(main_type, "myPlayer");
            g_new_text_method = patchlib_type_get_method_by_param_count(main_type, "NewText", 5);
            if (!g_new_text_method) g_new_text_method = patchlib_type_get_method_by_param_count(main_type, "NewText", 4);
        }

        LOG_INFO("[debug] /reload hooks: frame={} chat={} send={}",
                 static_cast<int>(g_hook_frame), static_cast<int>(g_hook_chat), static_cast<int>(g_hook_send));
    }

    void uninstall_debug_hooks() {
        if (g_hook_frame != PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(g_hook_frame);
        if (g_hook_chat != PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(g_hook_chat);
        if (g_hook_send != PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(g_hook_send);
        g_hook_frame = g_hook_chat = g_hook_send = PATCH_HOOK_INVALID_ID;
    }
} // namespace

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

    // 读取加载器隐藏配置（<private_dir>/config.json），按需开启调试工具
    if (ml_entry && ml_entry->private_dir) {
        // 推导 mods 根目录： private_dir = <workdir>/modloader/private/<pkgid>
        {
            const std::filesystem::path p(ml_entry->private_dir);
            const std::filesystem::path pkg = p.filename();
            const std::filesystem::path workdir = p.parent_path().parent_path().parent_path();
            g_mods_root = (workdir / "mods" / pkg).string();
            LOG_INFO("[debug] mods root: {}", g_mods_root);
        }
        try {
            load_config(ml_entry->private_dir);
        } catch (const std::exception &e) {
            LOG_WARN("[debug] config init failed: {}", e.what());
        }
        if (g_debug) install_debug_hooks();
    }

#ifdef LUALOADER_GUI
    // 原生 GUI 对 Mod 永久开放：任何定义了 mod.on_gui 的 Mod 都会被绘制。
    // 绘制走游戏自身的 XNA/Unity SpriteBatch，不再需要独立渲染后端。
    gui::install();
#endif

    LOG_INFO("LuaLoader initialized successfully");
    return ML_SUCCESS;
}

ml_result_t lualoader::core::cleanup_ml(ml_entry_t *ml_entry) {
    LOG_INFO("Cleaning up LuaLoader...");

    uninstall_debug_hooks();

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
            17,
<<<<<<< HEAD
            "1.9.6-ui-test",
=======
            "1.7.1",
>>>>>>> 57fb66f187dc4a5992925ae4b3bf26c2f9a21462
            1,
            0,
            nullptr,
    };
    return &info;
}
