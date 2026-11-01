/*******************************************************************************
 * LuaLoader - static Mod repository client
 *******************************************************************************/

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace lualoader::repository {

struct package_entry {
    std::string pkg_id;
    std::string download_url;
    std::string sha256;
    bool native{false};
};

struct repository_index {
    int schema_version{0};
    std::string repository;
    std::vector<package_entry> packages;
};

bool load_index(const std::filesystem::path &index_path,
                repository_index &out,
                std::string &error);

bool resolve_download_url(const std::string &index_url,
                          const package_entry &entry,
                          std::string &out,
                          std::string &error);

bool verify_downloaded_package(const std::filesystem::path &path,
                               const std::string &expected_sha256,
                               std::string &error);

} // namespace lualoader::repository
