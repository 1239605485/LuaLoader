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

#include "logger.hpp"

#include <cstdarg>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/android_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

static spdlog::level::level_enum convert_log_level(const mod_log_level_t level) {
    switch (level) {
        case MOD_LOG_LEVEL_TRACE:    return spdlog::level::trace;
        case MOD_LOG_LEVEL_DEBUG:    return spdlog::level::debug;
        case MOD_LOG_LEVEL_INFO:     return spdlog::level::info;
        case MOD_LOG_LEVEL_WARNING:  return spdlog::level::warn;
        case MOD_LOG_LEVEL_ERROR:    return spdlog::level::err;
        case MOD_LOG_LEVEL_CRITICAL: return spdlog::level::critical;
        case MOD_LOG_LEVEL_FATAL:    return spdlog::level::critical;
        default:                     return spdlog::level::info;
    }
}

void lualoader::logger::init() {
    try {
        if (auto existing_logger = spdlog::get("LuaLoader")) {
            g_logger = existing_logger;
            return;
        }

        std::vector<spdlog::sink_ptr> sinks;

#ifdef __ANDROID__
        const auto android_sink = std::make_shared<spdlog::sinks::android_sink_mt>("LuaLoader");
        android_sink->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v%$");
        sinks.push_back(android_sink);
#else
        const auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        stdout_sink->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v%$");
        sinks.push_back(stdout_sink);
#endif

        auto logger = std::make_shared<spdlog::logger>("LuaLoader", begin(sinks), end(sinks));
#if !defined(NDEBUG)
        logger->set_level(spdlog::level::trace);
#else
        logger->set_level(spdlog::level::info);
#endif

        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);

        logger->flush_on(spdlog::level::info);
        g_logger = logger;

        LOG_INFO("LuaLoader logger initialized successfully.");
    } catch (const spdlog::spdlog_ex &ex) {
        spdlog::error("Logger initialization failed: {}", ex.what());
        throw;
    }
}

void lualoader::logger::shutdown() {
    if (g_logger) {
        LOG_INFO("Shutting down logger...");
        g_logger->flush();
    }
    spdlog::drop("LuaLoader");
    g_logger.reset();
}

void lualoader::logger::init_mod() {
    try {
        if (auto existing_logger = spdlog::get("LuaLoaderMod")) {
            g_mod_logger = existing_logger;
            return;
        }

        std::vector<spdlog::sink_ptr> sinks;

#ifdef __ANDROID__
        const auto android_sink = std::make_shared<spdlog::sinks::android_sink_mt>("LuaLoaderMod");
        android_sink->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v%$");
        sinks.push_back(android_sink);
#else
        const auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        stdout_sink->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v%$");
        sinks.push_back(stdout_sink);
#endif

        auto logger = std::make_shared<spdlog::logger>("LuaLoaderMod", begin(sinks), end(sinks));
        logger->set_level(spdlog::level::trace);

        spdlog::register_logger(logger);
        logger->flush_on(spdlog::level::info);
        g_mod_logger = logger;

        LOG_INFO("LuaLoader mod logger initialized successfully.");
    } catch (const spdlog::spdlog_ex &ex) {
        spdlog::error("Mod logger initialization failed: {}", ex.what());
        throw;
    }
}

void lualoader::logger::shutdown_mod() {
    if (g_mod_logger) {
        g_mod_logger->flush();
    }
    spdlog::drop("LuaLoaderMod");
    g_mod_logger.reset();
}

void lualoader::logger::write(const mod_log_level_t level, const char *tag, const char *message) {
    if (!message) return;

    auto logger = g_mod_logger ? g_mod_logger : g_logger;
    if (!logger) return;

    std::string text = (tag && tag[0] != '\0') ? std::string("[") + tag + "] " + message : message;

    switch (convert_log_level(level)) {
        case spdlog::level::trace:    logger->trace(text); break;
        case spdlog::level::debug:    logger->debug(text); break;
        case spdlog::level::info:     logger->info(text); break;
        case spdlog::level::warn:     logger->warn(text); break;
        case spdlog::level::err:      logger->error(text); break;
        case spdlog::level::critical: logger->critical(text); break;
        default:                      logger->info(text); break;
    }

    if (level == MOD_LOG_LEVEL_FATAL) {
        logger->flush();
    }
}

void lualoader::logger::mod_logger_write_imp(const mod_log_level_t level, const char *tag, const char *fmt, ...) {
    if (!fmt) return;

    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    write(level, tag, buffer);
}
