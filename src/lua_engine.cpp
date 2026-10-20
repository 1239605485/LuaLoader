/*******************************************************************************
 * LuaLoader - lua engine
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

#include "lua_engine.hpp"
#include "lua_api.hpp"
#include "mod_api.hpp"
#include "lualoader_mod.h"

#include <atomic>
#include <cctype>
#include <map>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "logger.hpp"
#include "platform.hpp"

namespace lualoader::lua_engine {

    static int traceback(lua_State *L) {
        const char *msg = lua_tostring(L, 1);
        if (msg) {
            luaL_traceback(L, L, msg, 1);
        } else {
            lua_pushliteral(L, "<error object>");
        }
        return 1;
    }

    /// 打开全部 Lua 标准库（base/table/string/math/utf8/coroutine/io/os/debug/package）。
    /// 说明：LuaLoader 不再限制 Lua 能力；Mod 是否可信由玩家自行审查 Mod 包决定。
    static void open_all_libs(lua_State *L) {
        luaL_openlibs(L);
    }

    /// 找到 Mod 的原生库目录列表（按平台_架构，其次平台，再次 mod 根）
    static std::vector<std::string> native_dirs(const std::string &mod_dir,
                                                const std::string &private_dir,
                                                const std::string &platform,
                                                const std::string &arch);

#if defined(__ANDROID__)
    /// 当前进程（游戏）的应用私有目录：/data/data/<pkg>/files
    /// Android 的 linker 命名空间只允许 /data、/mnt/expand，/storage 下的 .so 无法 dlopen，
    /// 因此需要把原生库复制到这里再加载。
    static std::string android_app_private_dir() {
        std::ifstream cmd("/proc/self/cmdline", std::ios::binary);
        std::string pkg;
        if (cmd) std::getline(cmd, pkg, '\0');
        if (pkg.empty()) return {};
        return "/data/data/" + pkg + "/files";
    }

    /// 把自身（loader）提升到 RTLD_GLOBAL：
    /// 内核用 RTLD_LOCAL 从 memfd 加载 loader，导致 dlopen 的模块解析不到 loader 导出的
    /// lua_* / patchlib_* 符号。用 SONAME + RTLD_NOLOAD|RTLD_GLOBAL 可把已加载的自身提升为全局。
    static void promote_self_global() {
        static bool tried = false;
        if (tried) return;
        tried = true;
#if defined(__ANDROID__)
        const std::string soname =
                std::string("libloader.") + LUALOADER_PLATFORM_NAME + "." + LUALOADER_ARCH_NAME + ".so";
        void *h = dlopen(soname.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
        if (h) {
            LOG_INFO("[native] 已将自身提升为 RTLD_GLOBAL ({}): {}", soname, h);
        } else {
            const char *e = dlerror();
            LOG_WARN("[native] 提升自身为 GLOBAL 失败: {} ({})", soname, e ? e : "?");
        }
#endif
    }
#endif

    // Preserve the search order and publish complete files without truncating mapped libraries.
    static bool stage_native_libs(const std::vector<std::string> &src_dirs,
                                  const std::string &dst_dir, const std::string &mod_id) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto failed = [&](const char *action) {
            LOG_WARN("[mod {}] native staging {} failed: {}", mod_id, action, ec.message());
            return false;
        };
        std::map<std::string, fs::path> sources;
        for (const auto &dir: src_dirs) {
            if (!fs::is_directory(dir, ec)) {
                if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory)
                    return failed("scan");
                continue;
            }
            fs::directory_iterator it(dir, ec), end;
            if (ec) return failed("scan");
            while (it != end) {
                const bool regular = it->is_regular_file(ec);
                if (ec) return failed("stat");
                if (regular && it->path().extension() == ".so")
                    sources.emplace(it->path().filename().string(), it->path()); // first match wins
                it.increment(ec);
                if (ec) return failed("scan");
            }
        }
        fs::create_directories(dst_dir, ec);
        if (ec) return failed("mkdir");
        static std::atomic<unsigned long long> sequence{0};
        fs::path temporary;
        bool created = false;
        for (int i = 0; i < 128 && !created; ++i) {
            temporary = fs::path(dst_dir) / (".stage-" + std::to_string(++sequence));
            created = fs::create_directory(temporary, ec);
            if (ec) return failed("temporary directory");
        }
        if (!created) {
            LOG_WARN("[mod {}] could not allocate native staging directory", mod_id);
            return false;
        }
        struct Cleanup {
            fs::path path;
            ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
        } cleanup{temporary};
        // Copy every chosen file before changing the loadable directory.
        for (const auto &source: sources) {
            fs::copy_file(source.second, temporary / source.first, ec);
            if (ec) return failed("copy");
        }
        for (const auto &source: sources) {
            // Android/POSIX rename replaces the directory entry, leaving old mappings intact.
            fs::rename(temporary / source.first, fs::path(dst_dir) / source.first, ec);
            if (ec) return failed("publish");
        }
        // A removed or renamed library must not be preloaded on the next launch.
        fs::directory_iterator it(dst_dir, ec), end;
        if (ec) return failed("prune scan");
        while (it != end) {
            const auto path = it->path();
            const bool regular = it->is_regular_file(ec);
            if (ec) return failed("prune stat");
            if (regular && path.extension() == ".so" && !sources.count(path.filename().string())) {
                fs::remove(path, ec);
                if (ec) return failed("prune");
            }
            it.increment(ec);
            if (ec) return failed("prune scan");
        }
        return true;
    }

    /// 准备可加载的原生库目录：
    /// - Android：把各原生库复制到应用私有目录（规避 linker namespace 限制），返回该目录
    /// - 其它平台：直接返回原始目录
    static std::vector<std::string> prepare_native_libs(const std::string &mod_dir,
                                                        const std::string &private_dir,
                                                        const std::string &platform,
                                                        const std::string &arch,
                                                        const std::string &mod_id) {
        std::vector<std::string> src_dirs = native_dirs(mod_dir, private_dir, platform, arch);
#if defined(__ANDROID__)
        promote_self_global();
        const std::string base = android_app_private_dir();
        if (base.empty()) return src_dirs; // 拿不到私有目录就退回原路径（大概率仍会失败）
        std::string safe_id = mod_id;
        for (char &c: safe_id) if (!isalnum((unsigned char) c) && c != '.' && c != '_' && c != '-') c = '_';
        const std::string dst_dir = base + "/lualoader_native/" + safe_id;
        if (stage_native_libs(src_dirs, dst_dir, mod_id)) return {dst_dir};
        return {}; // Never load stale cached libraries after staging fails.
#else
        (void) mod_id;
        return src_dirs;
#endif
    }

    /// 配置 package：把 Mod 目录加入 package.path（保留系统默认路径）。
    /// 同时开放 C 原生模块：把 Mod 的平台原生目录加入 package.cpath，并保留 loadlib。
    /// 兼容说明：纯 Lua Mod 不受影响；原生模块为可选，仅当 Mod 自带对应平台 .so/.dll 时才会被加载。
    static void configure_package(lua_State *L, const std::string &mod_dir,
                                  const std::vector<std::string> &native_search_dirs) {
        lua_getglobal(L, "package");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }

        // 在默认搜索路径前追加 Mod 目录，保证 require 能加载 Mod 自带的 Lua 模块
        lua_getfield(L, -1, "path");
        const char *default_path = lua_tostring(L, -1);
        const std::string mod_pattern = mod_dir + "/?.lua;" + mod_dir + "/?/init.lua;";
        const std::string pattern = mod_pattern + (default_path ? default_path : "");
        lua_pop(L, 1);
        lua_pushstring(L, pattern.c_str());
        lua_setfield(L, -2, "path");

        // 开放原生模块：Mod 平台原生目录优先
        std::string native_pattern;
        for (const auto &dir: native_search_dirs) {
#ifdef _WIN32
            native_pattern += dir + "/?.dll;";
#else
            native_pattern += dir + "/?.so;";
#endif
        }
        lua_getfield(L, -1, "cpath");
        const char *default_cpath = lua_tostring(L, -1);
        lua_pop(L, 1);
        const std::string cpath = native_pattern + (default_cpath ? default_cpath : "");
        lua_pushstring(L, cpath.c_str());
        lua_setfield(L, -2, "cpath");
        // 保留 package.loadlib（open_all_libs 已提供，这里不再置空）
        lua_pop(L, 1);
    }

    /// 找到 Mod 的原生库目录列表（按平台_架构，其次平台，再次 mod 根）
    static std::vector<std::string> native_dirs(const std::string &mod_dir,
                                                const std::string &private_dir,
                                                const std::string &platform,
                                                const std::string &arch) {
        std::vector<std::string> dirs;
        const std::string pa = platform + "_" + arch;
        for (const std::string &base: {private_dir, mod_dir}) {
            if (base.empty()) continue;
            dirs.push_back(base + "/native/" + pa);
            dirs.push_back(base + "/native/" + platform);
            dirs.push_back(base + "/lib/native/" + pa);
            dirs.push_back(base + "/lib/native/" + platform);
        }
        return dirs;
    }

    /// 预加载原生库并注入 LualoaderMod API 表。
    /// Android 上模块无法通过 ELF 作用域解析 lua_*/patchlib_*，统一走 ll_set_api 注入；
    /// 桌面端同时保留 RTLD_GLOBAL，兼容直接 extern 的写法。
    static void preload_native_libs(const std::vector<std::string> &search_dirs,
                                    const std::string &mod_id,
                                    std::vector<void *> &loaded) {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const auto &dir: search_dirs) {
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto &entry: fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
#ifdef _WIN32
                if (ext != ".dll") continue;
                HMODULE h = LoadLibraryA(entry.path().string().c_str());
                if (!h) {
                    LOG_WARN("[mod {}] LoadLibrary failed: {}", mod_id, entry.path().string());
                    continue;
                }
                auto set_api = reinterpret_cast<void (*)(const ll_api_t *)>(
                        reinterpret_cast<void *>(GetProcAddress(h, "ll_set_api")));
                if (set_api) {
                    set_api(lualoader::mod_api::get_api());
                    LOG_INFO("[mod {}] injected ll_api ({} symbols)", mod_id,
                             lualoader::mod_api::symbol_count());
                }
                loaded.push_back(reinterpret_cast<void *>(h));
                LOG_INFO("[mod {}] preloaded native module: {}", mod_id, entry.path().filename().string());
#else
                if (ext != ".so") continue;
                void *h = dlopen(entry.path().c_str(), RTLD_NOW | RTLD_GLOBAL);
                if (!h) {
                    const char *e = dlerror();
                    LOG_WARN("[mod {}] dlopen failed: {} ({})", mod_id, entry.path().string(), e ? e : "?");
                    continue;
                }
                auto set_api = reinterpret_cast<void (*)(const ll_api_t *)>(
                        dlsym(h, "ll_set_api"));
                if (set_api) {
                    set_api(lualoader::mod_api::get_api());
                    LOG_INFO("[mod {}] injected ll_api ({} symbols)", mod_id,
                             lualoader::mod_api::symbol_count());
                }
                loaded.push_back(h);
                LOG_INFO("[mod {}] preloaded native module: {}", mod_id, entry.path().filename().string());
#endif
            }
        }
    }

    static bool run_file(lua_State *L, const std::string &path, std::string &err) {
        if (luaL_loadfile(L, path.c_str()) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            err = msg ? msg : "failed to load lua file";
            lua_pop(L, 1);
            return false;
        }

        lua_pushcfunction(L, traceback);
        lua_insert(L, 1);
        if (lua_pcall(L, 0, 0, 1) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            err = msg ? msg : "failed to run lua file";
            lua_pop(L, 2); // error + traceback function
            return false;
        }
        lua_remove(L, 1); // traceback function
        return true;
    }

    static void protected_call(lua_State *L, const int ref, const char *what) {
        if (ref == LUA_NOREF) return;
        const int base = lua_gettop(L);
        lua_pushcfunction(L, traceback);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, base + 1) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            LOG_ERROR("[lua] {} failed: {}", what, msg ? msg : "unknown error");
        }
        lua_settop(L, base);
    }

    /// 调用栈顶的函数（函数与参数已入栈），带 traceback。调用后函数/参数出栈。
    static void pcall_top(lua_State *L, const int nargs, const char *what) {
        const int funcidx = lua_gettop(L) - nargs;
        lua_pushcfunction(L, traceback);
        lua_insert(L, funcidx);
        const int status = lua_pcall(L, nargs, 0, funcidx);
        lua_remove(L, funcidx); // 移除 traceback
        if (status != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            LOG_ERROR("[lua] {} failed: {}", what, msg ? msg : "unknown error");
            lua_pop(L, 1);
        }
    }

    /// 执行全局表 name（若存在）中的每个条目：
    ///   - 函数值：直接调用
    ///   - 字符串：作为全局函数名查找后调用
    /// 其余类型与未知的函数名会被跳过（并给出警告）
    static void run_todo_list(lua_State *L, const char *name) {
        lua_getglobal(L, name);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        const int len = static_cast<int>(luaL_len(L, -1));
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isfunction(L, -1)) {
                pcall_top(L, 0, name);
                continue;
            }
            if (lua_type(L, -1) == LUA_TSTRING) {
                const std::string func_name = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getglobal(L, func_name.c_str());
                if (lua_isfunction(L, -1)) {
                    const std::string label = std::string(name) + " -> " + func_name;
                    pcall_top(L, 0, label.c_str());
                } else {
                    LOG_WARN("[lua] {} refers to unknown function '{}'", name, func_name);
                    lua_pop(L, 1);
                }
                continue;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    static int ref_global_field(lua_State *L, const char *key) {
        lua_getglobal(L, "mod");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return LUA_NOREF;
        }
        lua_getfield(L, -1, key);
        int ref = LUA_NOREF;
        if (lua_isfunction(L, -1)) {
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // mod
        return ref;
    }

    static bool read_string_field(lua_State *L, int table_idx, const char *key, std::string &out) {
        if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
        lua_getfield(L, table_idx, key);
        bool ok = false;
        if (lua_isstring(L, -1)) {
            out = lua_tostring(L, -1);
            ok = true;
        }
        lua_pop(L, 1);
        return ok;
    }

    static bool read_int_field(lua_State *L, int table_idx, const char *key, int &out) {
        if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
        lua_getfield(L, table_idx, key);
        bool ok = false;
        if (lua_isnumber(L, -1)) {
            out = static_cast<int>(lua_tointeger(L, -1));
            ok = true;
        }
        lua_pop(L, 1);
        return ok;
    }

    /// 读取 Lua 脚本中的 mod.meta 覆盖 mod.json 中的信息
    static void read_info(lua_State *L, lua_mod_handle_t *handle) {
        lua_getglobal(L, "mod");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        lua_getfield(L, -1, "meta");
        if (lua_istable(L, -1)) {
            std::string text;
            if (read_string_field(L, -1, "pkg_id", text)) {
                handle->info.pkg_id = text;
            } else if (read_string_field(L, -1, "name", text)) {
                handle->info.pkg_id = text;
            }
            if (read_string_field(L, -1, "version", text)) {
                handle->info.version = text;
            }
            read_int_field(L, -1, "version_code", handle->info.version_code);
            read_int_field(L, -1, "api_version", handle->info.api_version);
        }
        lua_pop(L, 1); // meta
        lua_pop(L, 1); // mod
    }

    /// 解析入口脚本路径。优先使用内核部署 Mod 资源的位置 <private_dir>/lib/，
    /// 与 KernelLoader 查找动态库的约定保持一致，并提供若干回退路径便于本地开发。
    static std::vector<std::filesystem::path> entry_candidates(const lua_mod_handle_t *handle,
                                                               const std::string &config_dir) {
        const std::vector<std::filesystem::path> dirs = {
                std::filesystem::path(handle->private_dir) / "lib",
                std::filesystem::path(handle->private_dir),
                std::filesystem::path(config_dir),
                std::filesystem::path(config_dir) / "Resources" / "lib",
        };
        std::vector<std::filesystem::path> candidates;
        for (const auto &dir: dirs) {
            candidates.push_back(dir / handle->entry);
        }
        return candidates;
    }

    bool load(lua_mod_handle_t *handle,
              const std::string &config_path,
              const std::string &private_dir,
              std::string &err) {
        handle->private_dir = private_dir;
        handle->platform = LUALOADER_PLATFORM_NAME;
        handle->arch = LUALOADER_ARCH_NAME;
        if (handle->entry.empty()) handle->entry = "main.lua";

        const std::string config_dir = std::filesystem::path(config_path).parent_path().string();
        std::string entry_path;
        for (const auto &candidate: entry_candidates(handle, config_dir)) {
            if (std::filesystem::exists(candidate)) {
                entry_path = candidate.string();
                break;
            }
        }
        if (entry_path.empty()) {
            err = "entry script not found: " + (std::filesystem::path(private_dir) / "lib" / handle->entry).string();
            return false;
        }
        handle->mod_dir = std::filesystem::path(entry_path).parent_path().string();

        lua_State *L = luaL_newstate();
        if (!L) {
            err = "failed to create Lua state";
            return false;
        }
        handle->L = L;

        open_all_libs(L);
        const std::vector<std::string> native_search = prepare_native_libs(
                handle->mod_dir, private_dir, handle->platform, handle->arch, handle->mod_id);
        configure_package(L, handle->mod_dir, native_search);
        preload_native_libs(native_search, handle->mod_id, handle->native_libs);
        lua_api::register_api(L, handle);

        LOG_INFO("Running Lua entry: {}", entry_path);
        if (!run_file(L, entry_path, err)) {
            LOG_ERROR("Failed to run Lua mod {}: {}", handle->mod_id, err);
            // Unregister item/projectile callbacks while their Lua state is still alive.
            // Use the same cleanup order as a normal unload, including native libraries.
            close(handle);
            return false;
        }

        read_info(L, handle);
        handle->init_ref = ref_global_field(L, "init");
        handle->cleanup_ref = ref_global_field(L, "cleanup");
        handle->gui_ref = ref_global_field(L, "on_gui");
        return true;
    }

    void call_init(lua_mod_handle_t *handle) {
        if (!handle || !handle->L) return;
        lua_State *L = handle->L;

        // 平台化任务清单，按“通用 -> 平台 -> 平台+架构”的顺序执行
        run_todo_list(L, "todo_list");
        const std::string platform_list = "todo_list_" + handle->platform;
        run_todo_list(L, platform_list.c_str());
        const std::string platform_arch_list = "todo_list_" + handle->platform + "_" + handle->arch;
        run_todo_list(L, platform_arch_list.c_str());

        protected_call(L, handle->init_ref, "mod.init");
    }

    void call_cleanup(lua_mod_handle_t *handle) {
        if (!handle || !handle->L) return;
        protected_call(handle->L, handle->cleanup_ref, "mod.cleanup");
    }

    void call_on_gui(lua_mod_handle_t *handle) {
        if (!handle || !handle->L) return;
        protected_call(handle->L, handle->gui_ref, "mod.on_gui");
    }

    void uninstall_hooks(lua_mod_handle_t *handle) {
        lua_api::uninstall_all_hooks(handle);
    }

    /// 关闭该 Mod 预加载的所有原生库（逆序）。Lua 状态机关闭之后再调用，
    /// 避免库代码在 finalizer / hook 卸载过程中被回收。
    static void unload_native_libs(std::vector<void *> &libs) {
        for (auto it = libs.rbegin(); it != libs.rend(); ++it) {
            if (!*it) continue;
#ifdef _WIN32
            FreeLibrary(reinterpret_cast<HMODULE>(*it));
#else
            dlclose(*it);
#endif
        }
        libs.clear();
    }

    void close(lua_mod_handle_t *handle) {
        if (!handle) return;
        lua_api::uninstall_all_hooks(handle);
        lua_api::unregister_items(handle);
        lua_api::unregister_projectiles(handle);
        if (handle->L) {
            lua_close(handle->L);
            handle->L = nullptr;
        }
        unload_native_libs(handle->native_libs);
        handle->init_ref = LUA_NOREF;
        handle->cleanup_ref = LUA_NOREF;
        handle->gui_ref = LUA_NOREF;
    }

} // namespace lualoader::lua_engine
