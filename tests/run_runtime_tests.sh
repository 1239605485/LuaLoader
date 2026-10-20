#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
for source in "$root"/lib/lua-5.4.8/src/*.c; do
    gcc -O0 -g -fPIC -DLUA_USE_DLOPEN -c "$source" -o "$build/$(basename "${source%.c}").o"
done
gcc -I"$root/includes" -c "$root/includes/tefkernel/tef_api_imp.c" -o "$build/tef_api.o"
g++ -std=c++17 -O0 -g -I"$root/includes" -I"$root/lib/lua-5.4.8/src" \
    -I"$root/lib/spdlog-1.17.0/include" "$root/tests/runtime_lifetime_test.cpp" \
    "$root/src/mod_api.cpp" "$root/src/logger.cpp" "$root/src/image_decode.cpp" \
    "$build"/*.o -ldl -lm -pthread -o "$build/runtime_test"
failed=0
for case in hook_reuse load_failure null_private duplicate callback_item_defaults callback_can_use callback_projectile_defaults callback_ai result_prefix_bad result_postfix_bad result_prefix_valid result_postfix_valid native_priority native_stale; do
    if ! "$build/runtime_test" "$case" "$build/$case"; then failed=1; fi
done
exit "$failed"
