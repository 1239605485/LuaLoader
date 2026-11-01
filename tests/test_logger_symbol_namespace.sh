#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if rg -n 'extern "C" void mod_logger_write_imp|void lualoader::logger::mod_logger_write_imp' \
    "$root/includes/logger.hpp" "$root/src/core/logger.cpp"; then
    echo "LuaLoader still defines the colliding mod_logger_write_imp symbol" >&2
    exit 1
fi

rg -q 'lualoader_mod_logger_write_imp' "$root/includes/logger.hpp"
rg -q 'lualoader_mod_logger_write_imp' "$root/src/core/logger.cpp"
