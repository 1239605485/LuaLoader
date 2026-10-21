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

#pragma once

#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace lualoader {

    /// 从 mod.json / Lua 脚本收集到的 Mod 信息
    struct lua_mod_info_t {
        std::string pkg_id{"unknown"};
        int version_code{1};
        std::string version{"1.0.0"};
        int api_version{1};
    };

    /// 单个 Lua Mod 的运行时句柄
    struct lua_mod_handle_t {
        std::string mod_id;      ///< Mod 唯一标识符（由内核分配）
        std::string mod_dir;     ///< 存放 mod.json 与 lua 源文件的目录
        std::string private_dir; ///< Mod 私有数据目录（read_file / write_file 的根）
        std::string logs_dir;    ///< Mod 日志目录（内核传入）
        std::string config_path; ///< Mod 配置文件路径（重载时重建 manifest 用）
        std::string entry;       ///< 入口 lua 文件（相对 <private_dir>/lib）
        std::string platform;    ///< 运行平台（android/linux/windows/macos/ios/unknown）
        std::string arch;        ///< 运行架构（arm64/arm/x64/x86/unknown）
        lua_State *L{nullptr};   ///< 该 Mod 独立的 Lua 状态机
        lua_mod_info_t info;
        int init_ref{LUA_NOREF};    ///< mod.init 的注册表引用
        int cleanup_ref{LUA_NOREF}; ///< mod.cleanup 的注册表引用
        int gui_ref{LUA_NOREF};     ///< mod.on_gui 的注册表引用（原生即时模式 GUI）
        int net_recv_ref{LUA_NOREF}; ///< mod.net.on_receive 的注册表引用（服务端收包回调）
        std::vector<int> hook_slots; ///< 该 Mod 申请的钩子槽位（用于卸载时清理）
        std::vector<void *> native_libs; ///< 预加载的原生库句柄（卸载时关闭，避免残留累积）
    };

    namespace lua_engine {

        /**
         * @brief 创建 Lua 状态机、注入 mod API 并执行入口脚本
         *
         * @param handle 已填充 mod_id/mod_dir/private_dir/entry/info 的句柄（L 字段由本函数创建）
         * @param config_path mod.json 的完整路径（仅用于日志与目录推导）
         * @param private_dir Mod 私有数据目录
         * @param err 失败时输出错误信息
         * @return 成功返回 true
         */
        bool load(lua_mod_handle_t *handle,
                  const std::string &config_path,
                  const std::string &private_dir,
                  std::string &err);

        /// 调用 Mod 的 mod.init 回调（若存在）
        void call_init(lua_mod_handle_t *handle);

        /// 调用 Mod 的 mod.cleanup 回调（若存在）
        void call_cleanup(lua_mod_handle_t *handle);

        /// 调用 Mod 的 mod.on_gui 回调（若存在）；在原生 GUI 帧内执行
        void call_on_gui(lua_mod_handle_t *handle);

        /// 卸载该 Mod 安装的所有钩子
        void uninstall_hooks(lua_mod_handle_t *handle);

        /// 关闭 Lua 状态机并释放回调引用
        void close(lua_mod_handle_t *handle);

    } // namespace lua_engine

} // namespace lualoader
