#pragma once
#include <string>
struct lua_State;
namespace lualoader::text_input {
struct Options {
    std::string text, placeholder;
    int limit{0};
    bool multiline{false}, secure{false};
};
struct Result {
    std::string text, status{"idle"}, error;
};
bool available();
bool open(const std::string &owner, const Options &options, std::string &error);
Result read(const std::string &owner);
void close(const std::string &owner, bool commit = false);
void release_mod(const std::string &owner);
void shutdown();
void register_api(lua_State *L, const std::string &owner);
}
