#include "system_keyboard.hpp"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/string.h"
#include <cstdlib>
#include <climits>
#include <cstdint>
#if defined(__ANDROID__)
#include <dlfcn.h>
#include "xdl.h"
#endif
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace lualoader::text_input {
namespace {
struct Backend {
    patch_handle_t open{}, text{}, status{}, active{};
    int argc{};
    bool attempted{};
#if defined(__ANDROID__)
    uint32_t (*pin)(void *, bool){};
    void (*unpin)(uint32_t){};
#endif
} backend;
struct Session {
    std::string owner, original;
    Result result;
    patch_handle_t keyboard{};
    uint32_t root{};
} session;

bool resolve() {
#if defined(__ANDROID__) || defined(LUALOADER_KEYBOARD_TEST)
    if (backend.attempted) {
#if defined(__ANDROID__)
        if (!backend.pin || !backend.unpin) return false;
#endif
        return backend.open && backend.text && backend.status && backend.active;
    }
    if (!patchlib_type_get_type || !patchlib_type_get_method_by_param_count ||
        !patchlib_method_invoke_args || !patchlib_string_create ||
        !patchlib_string_cstr) return false;
    backend.attempted = true;
#if defined(__ANDROID__)
    void *library = xdl_open("libil2cpp.so", XDL_DEFAULT);
    if (library) {
        backend.pin = reinterpret_cast<uint32_t (*)(void *, bool)>(xdl_sym(library, "il2cpp_gchandle_new", nullptr));
        backend.unpin = reinterpret_cast<void (*)(uint32_t)>(xdl_sym(library, "il2cpp_gchandle_free", nullptr));
        xdl_close(library);
    }
    if (!backend.pin || !backend.unpin) return false;
#endif
    auto type = patchlib_type_get_type("UnityEngine", "TouchScreenKeyboard");
    if (!type) return false;
    backend.open = patchlib_type_get_method_by_param_count(type, "Open", 8);
    backend.argc = 8;
    if (!backend.open) {
        backend.open = patchlib_type_get_method_by_param_count(type, "Open", 7);
        backend.argc = 7;
    }
    backend.text = patchlib_type_get_method_by_param_count(type, "get_text", 0);
    backend.status = patchlib_type_get_method_by_param_count(type, "get_status", 0);
    backend.active = patchlib_type_get_method_by_param_count(type, "set_active", 1);
    patchlib_free(type);
    return backend.open && backend.text && backend.status && backend.active;
#else
    return false;
#endif
}
void release_keyboard() {
#if defined(__ANDROID__)
    if (session.root && backend.unpin) backend.unpin(session.root);
#endif
    session.root = 0;
    if (session.keyboard) patchlib_free(session.keyboard);
    session.keyboard = nullptr;
}
void update() {
    if (!session.keyboard || session.result.status != "visible") return;
    patch_handle_t text{};
    int status = 0;
    bool ok = patchlib_method_invoke_args(backend.text, session.keyboard, &text, nullptr);
    if (ok && text) {
        char *value = patchlib_string_cstr(text);
        if (value) { session.result.text = value; std::free(value); }
        else ok = false;
    }
    if (text) patchlib_free(text);
    if (!ok || !patchlib_method_invoke_args(backend.status, session.keyboard, &status, nullptr)) {
        bool active = false;
        void *args[] = {&active};
        patchlib_method_invoke_args(backend.active, session.keyboard, nullptr, args);
        session.result.status = "error";
        session.result.error = "cannot read native keyboard state";
        release_keyboard();
        return;
    }
    switch (status) {
        case 0: return;
        case 1: session.result.status = "done"; break;
        case 2: session.result.status = "canceled"; session.result.text = session.original; break;
        case 3: session.result.status = "lost_focus"; break;
        default: session.result.status = "error"; session.result.error = "unknown native keyboard status"; break;
    }
    release_keyboard();
}
std::string lua_owner(lua_State *L) { return lua_tostring(L, lua_upvalueindex(1)); }
int l_available(lua_State *L) { lua_pushboolean(L, available()); return 1; }
int l_open(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    for (const char *key : {"text", "placeholder"}) {
        lua_getfield(L, 1, key);
        if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TSTRING)
            return luaL_error(L, "%s must be a string", key);
        lua_pop(L, 1);
    }
    lua_getfield(L, 1, "limit");
    lua_Integer limit = luaL_optinteger(L, -1, 0);
    if (limit < 0 || limit > INT_MAX) return luaL_error(L, "invalid text limit");
    lua_pop(L, 1);
    Options options;
    for (const char *key : {"text", "placeholder"}) {
        lua_getfield(L, 1, key);
        if (lua_type(L, -1) == LUA_TSTRING)
            (key[0]=='t' ? options.text : options.placeholder) = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    options.limit = static_cast<int>(limit);
    lua_getfield(L, 1, "multiline"); options.multiline = lua_toboolean(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "secure"); options.secure = lua_toboolean(L, -1); lua_pop(L, 1);
    std::string error;
    bool ok = open(lua_owner(L), options, error);
    lua_pushboolean(L, ok);
    if (error.empty()) lua_pushnil(L); else lua_pushlstring(L, error.data(), error.size());
    return 2;
}
int l_read(lua_State *L) {
    auto result = read(lua_owner(L));
    lua_pushlstring(L, result.text.data(), result.text.size());
    lua_pushstring(L, result.status.c_str());
    if (result.error.empty()) lua_pushnil(L); else lua_pushstring(L, result.error.c_str());
    return 3;
}
int l_close(lua_State *L) { close(lua_owner(L), lua_toboolean(L, 1)); return 0; }
}
bool available() { return resolve(); }
bool open(const std::string &owner, const Options &options, std::string &error) {
    error.clear();
    if (!resolve()) { error = "native text input is unavailable in this game build/platform"; return false; }
    if (options.limit < 0) { error = "invalid text limit"; return false; }
    update();
    if (session.keyboard && session.owner != owner) { error = "native text input is busy"; return false; }
    close(owner, false);
    if (session.keyboard) { error = "cannot replace active native keyboard"; return false; }
    session = Session{};
    session.owner = owner;
    session.original = options.text;
    session.result.text = options.text;
    patch_handle_t text = patchlib_string_create(options.text.c_str());
    patch_handle_t placeholder = patchlib_string_create(options.placeholder.c_str());
    if (!text || !placeholder) {
        if (text) patchlib_free(text);
        if (placeholder) patchlib_free(placeholder);
        error = "cannot create native input strings";
        session.result.status = "error";
        session.result.error = error;
        return false;
    }
    int keyboard_type = 0; // Default permits Chinese IMEs; ASCIICapable does not.
    bool autocorrect = true, alert = false;
    bool multiline = options.multiline, secure = options.secure;
    int limit = options.limit;
    void *args[] = {&text, &keyboard_type, &autocorrect, &multiline, &secure, &alert, &placeholder, &limit};
    bool ok = patchlib_method_invoke_args(backend.open, nullptr, &session.keyboard, args);
#if defined(__ANDROID__)
    if (ok && session.keyboard) {
        session.root = backend.pin(session.keyboard, false);
        if (!session.root) {
            bool active = false;
            void *close_args[] = {&active};
            patchlib_method_invoke_args(backend.active, session.keyboard, nullptr, close_args);
            ok = false;
        }
    }
#endif
    patchlib_free(text);
    patchlib_free(placeholder);
    if (!ok || !session.keyboard) {
        release_keyboard();
        error = "native keyboard Open failed";
        session.result.status = "error";
        session.result.error = error;
        return false;
    }
    session.result.status = "visible";
    return true;
}
Result read(const std::string &owner) {
    if (session.owner != owner) return {};
    update();
    return session.result;
}
void close(const std::string &owner, bool commit) {
    if (session.owner != owner || !session.keyboard) return;
    update();
    if (!session.keyboard) return;
    bool active = false;
    void *args[] = {&active};
    if (!patchlib_method_invoke_args(backend.active, session.keyboard, nullptr, args)) {
        session.result.status = "error";
        session.result.error = "cannot close native keyboard";
        return;
    }
    session.result.status = commit ? "done" : "canceled";
    if (!commit) session.result.text = session.original;
    release_keyboard();
}
void release_mod(const std::string &owner) {
    if (session.owner == owner || session.owner.rfind(owner + "#", 0) == 0) {
        close(session.owner, false);
        release_keyboard();
        session = Session{};
    }
}
void shutdown() {
    if (!session.owner.empty()) release_mod(session.owner);
    for (auto method : {backend.open, backend.text, backend.status, backend.active})
        if (method) patchlib_free(method);
    backend = Backend{};
}
void register_api(lua_State *L, const std::string &owner) {
    lua_newtable(L);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "api_version");
    const luaL_Reg functions[] = {{"available", l_available}, {"open", l_open},
                                  {"read", l_read}, {"close", l_close}, {nullptr, nullptr}};
    lua_pushlstring(L, owner.data(), owner.size());
    luaL_setfuncs(L, functions, 1);
    lua_setfield(L, -2, "text_input");
}
}
