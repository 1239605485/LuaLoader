#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ---------------- mod.net：联机同步 ----------------

    /// mod.net.mode() -> 0=单机 1=客户端 2=服务端
    static int l_net_mode(lua_State *L) {
        static patch_handle_t f_net_mode = PATCH_NULL;
        static patch_handle_t net_cls = PATCH_NULL;
        static bool tried = false;
        if (!tried) {
            tried = true;
            net_cls = patchlib_type_get_type("Terraria", "Main");
            if (patchlib_is_valid(net_cls)) {
                f_net_mode = patchlib_type_get_field(net_cls, "netMode");
                /* 桌面端 field 依附 class，net_cls 必须常驻（进程级，一个句柄）。 */
            }
        }
        int mode = 0;
        if (patchlib_is_valid(f_net_mode))
            patchlib_field_get_value(f_net_mode, PATCH_NULL, &mode);
        lua_pushinteger(L, mode);
        return 1;
    }

    /// mod.net.tile_kill(x, y[, no_item])
    /// 客户端请求服务端挖掉一个图格（服务端权威执行并广播）。
    static int l_net_tile_kill(lua_State *L) {
        const int x = static_cast<int>(luaL_checkinteger(L, 1));
        const int y = static_cast<int>(luaL_checkinteger(L, 2));
        const bool no_item = lua_toboolean(L, 3) != 0;
        terraria_net_send_tile_action(x, y, no_item ? 4 : 0);
        return 0;
    }

    /// mod.net.tile_square(x, y, width, height)
    static int l_net_tile_square(lua_State *L) {
        const int x = static_cast<int>(luaL_checkinteger(L, 1));
        const int y = static_cast<int>(luaL_checkinteger(L, 2));
        const int w = static_cast<int>(luaL_checkinteger(L, 3));
        const int h = static_cast<int>(luaL_checkinteger(L, 4));
        terraria_net_send_tile_square(x, y, w, h);
        return 0;
    }

    /// mod.net.send(channel, payload)
    /// 客户端 -> 服务端；服务端收到后按频道路由到 mod.net.on 注册的回调。
    static int l_net_send(lua_State *L) {
        lua_mod_handle_t *h = get_handle(L);
        if (!h) return luaL_error(L, "mod handle missing");
        const char *channel = luaL_checkstring(L, 1);
        size_t len = 0;
        const char *payload = luaL_optlstring(L, 2, "", &len);
        terraria_net_send_mod_message(h->mod_id.c_str(), channel, payload,
                                      static_cast<int>(len));
        return 0;
    }

    /// mod.net.on(channel, function(payload, player) end)
    /// 给某个频道注册处理回调，只在服务端（房主/专用服）触发。
    /// 传 nil 作为第二个参数可移除该频道的回调。
    static int l_net_on(lua_State *L) {
        lua_mod_handle_t *h = get_handle(L);
        if (!h) return luaL_error(L, "mod handle missing");
        const char *channel = luaL_checkstring(L, 1);

        auto it = h->net_recv_refs.find(channel);
        if (lua_isnil(L, 2)) {                       // on(channel, nil) = 移除
            if (it != h->net_recv_refs.end()) {
                luaL_unref(L, LUA_REGISTRYINDEX, it->second);
                h->net_recv_refs.erase(it);
            }
            return 0;
        }
        if (!lua_isfunction(L, 2))
            return luaL_error(L, "mod.net.on 第二个参数需要函数或 nil");

        if (it != h->net_recv_refs.end())            // 重复注册 = 覆盖
            luaL_unref(L, LUA_REGISTRYINDEX, it->second);
        lua_pushvalue(L, 2);
        h->net_recv_refs[channel] = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }

    static const luaL_Reg mod_net_functions[] = {
            {"mode",        l_net_mode},
            {"send",        l_net_send},
            {"on",          l_net_on},
            {"tile_kill",   l_net_tile_kill},
            {"tile_square", l_net_tile_square},
            {nullptr,       nullptr},
    };


void add_net(lua_State *L) {
    luaL_setfuncs(L, mod_net_functions, 0);
}

} // namespace lualoader::lua_api
