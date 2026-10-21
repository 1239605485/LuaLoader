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

#pragma once

#include <string>
#include <unordered_map>

#include "lua_engine.hpp"
#include "tefkernel/modloader/modloader_core.h"

namespace lualoader::core {
    inline std::unordered_map<std::string, lua_mod_handle_t *> lua_mod_handles{};
    inline std::unordered_map<std::string, multiplayer_mod_info_t *> lua_mod_multiplayer_infos{};

    ml_result_t load_mod(mod_manifest_t *mod_manifest);

    ml_result_t unload_mod(mod_manifest_t *mod_manifest);


    ml_result_t init_mod(mod_manifest_t *mod_manifest);

    const multiplayer_mod_info_t *get_multiplayer_info(mod_manifest_t *mod_manifest);

    ml_result_t init_ml(ml_entry_t *ml_entry);

    ml_result_t cleanup_ml(ml_entry_t *ml_entry);

    const ml_info_t *get_info();


    /// 依次调用所有 Mod 的 mod.on_gui（每帧渲染）
    void call_all_on_gui();
} // namespace lualoader::core
