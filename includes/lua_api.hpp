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

#pragma once

#include "lua_engine.hpp"

namespace lualoader::lua_api {

    /**
     * @brief 向 Lua 状态机注入全局 `mod` 表
     *
     * 提供以下能力：
     *  - 日志：mod.log / mod.trace / mod.debug / mod.info / mod.warn / mod.error / mod.fatal
     *  - 数据：mod.read_file / mod.write_file / mod.file_exists（限制在 private_dir 内）
     *  - 钩子：mod.patch.* 映射 tefkernel patchlib 的类型/字段/方法/钩子
     *  - 托管字符串：mod.patch.string_create / string_value / string_empty / string_length，
     *    以及字段/数组的 "string" 伪类型
     *  - 带参构造：mod.patch.new_instance(type, ...) / construct(ctor, ...)
     *  - 句柄生命周期：mod.patch.retain(handle) 返回由 Lua GC 托管的句柄副本；
     *    install_hook 支持 { copy = true }
     *
     * @param L Lua 状态机
     * @param handle 当前 Mod 句柄（保存于注册表，供 C 回调取用）
     */
    void register_api(lua_State *L, lua_mod_handle_t *handle);

    /// 卸载 handle 上安装的所有钩子并释放槽位
    void uninstall_all_hooks(lua_mod_handle_t *handle);

    /// 注销 handle 注册的所有自定义物品并释放回调引用
    void unregister_items(lua_mod_handle_t *handle);

    /// 注销 handle 注册的所有自定义弹幕并释放回调引用
    void unregister_projectiles(lua_mod_handle_t *handle);

} // namespace lualoader::lua_api
