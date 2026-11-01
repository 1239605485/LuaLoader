/*******************************************************************************
 * LuaLoader - logger
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

#include <spdlog/spdlog.h>
#include "../mod-api/mod_logger.h"

#if !defined(NDEBUG)
#define LOG_TRACE(...)    spdlog::trace("[{}:{}:{}] {}", __FILE__, __LINE__, __FUNCTION__, fmt::format(__VA_ARGS__))
#define LOG_DEBUG(...)    spdlog::debug("[{}:{}:{}] {}", __FILE__, __LINE__, __FUNCTION__, fmt::format(__VA_ARGS__))
#define LOG_INFO(...)     spdlog::info("[{}:{}:{}] {}", __FILE__, __LINE__, __FUNCTION__, fmt::format(__VA_ARGS__))
#define LOG_WARN(...)     spdlog::warn("[{}:{}:{}] {}", __FILE__, __LINE__, __FUNCTION__, fmt::format(__VA_ARGS__))
#define LOG_ERROR(...)    spdlog::error("[{}:{}:{}] {}", __FILE__, __LINE__, __FUNCTION__, fmt::format(__VA_ARGS__))
#define LOG_CRITICAL(...) spdlog::critical("[{}:{}:{}] {}", __FILE__, __LINE__, __FUNCTION__, fmt::format(__VA_ARGS__))
#else
#define LOG_TRACE(...)    spdlog::trace("{}", fmt::format(__VA_ARGS__))
#define LOG_DEBUG(...)    spdlog::debug("{}", fmt::format(__VA_ARGS__))
#define LOG_INFO(...)     spdlog::info("{}", fmt::format(__VA_ARGS__))
#define LOG_WARN(...)     spdlog::warn("{}", fmt::format(__VA_ARGS__))
#define LOG_ERROR(...)    spdlog::error("{}", fmt::format(__VA_ARGS__))
#define LOG_CRITICAL(...) spdlog::critical("{}", fmt::format(__VA_ARGS__))
#endif


namespace lualoader::logger {
    inline std::shared_ptr<spdlog::logger> g_logger{};
    inline std::shared_ptr<spdlog::logger> g_mod_logger{};

    void init();

    void shutdown();

    void init_mod();

    void shutdown_mod();

    /**
     * @brief 供 Lua 侧 mod.log 使用的统一写入接口
     * @param level 日志级别
     * @param tag 标签（通常为 mod_id）
     * @param message 已格式化好的消息
     */
    void write(mod_log_level_t level, const char* tag, const char* message);

    // Keep the loader callback symbol private to LuaLoader's ABI namespace.
    // KernelLoader exposes a function with the old generic C name; both loaders
    // can be injected into the same process, so a generic symbol is unsafe.
    extern "C" void lualoader_mod_logger_write_imp(mod_log_level_t level, const char* tag, const char* fmt, ...);
}
