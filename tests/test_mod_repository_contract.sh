#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT

cat >"$test_bin.cpp" <<'CPP'
#include "mod_repository.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lualoader-mod-repository-contract";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "index.json") << R"({
      "schemaVersion": 1,
      "mods": [{"pkgId":"demo.mod","downloadUrl":"dist/demo.mod.tefpkg","sha256":"","native":false}]
    })";
    lualoader::repository::repository_index index;
    std::string error;
    assert(lualoader::repository::load_index(dir / "index.json", index, error));
    assert(index.packages.size() == 1);
    assert(index.packages[0].pkg_id == "demo.mod");
    std::string resolved;
    assert(lualoader::repository::resolve_download_url(
        "https://example.com/repository/index.json", index.packages[0], resolved, error));
    assert(resolved == "https://example.com/repository/dist/demo.mod.tefpkg");
    std::ofstream(dir / "demo.mod.tefpkg", std::ios::binary) << "abc";
    assert(lualoader::repository::verify_downloaded_package(dir / "demo.mod.tefpkg",
        "", error) == false); // not a ZIP archive
    std::ofstream(dir / "demo.mod.tefpkg", std::ios::binary) << "PK\x03\x04";
    assert(lualoader::repository::verify_downloaded_package(dir / "demo.mod.tefpkg",
        "8dcc7e601606217f3b754766511182a916b17e9a26a94c9d887104eba92e9bb2", error));
    fs::remove_all(dir);
}
CPP

g++ -std=c++17 -I"$root/includes" "$test_bin.cpp" "$root/src/core/mod_repository.cpp" -o "$test_bin.out"
"$test_bin.out"
