#include "lauxlib.h"
#include "support_api.h"
#include "tefkernel/terraria/item_slot.h"
#include "tefkernel/terraria/recipe_query.h"
#include "tefkernel/terraria/world_entity.h"
#include <limits.h>
#include <string.h>
#define CONTEXT_KEY "lualoader.support.state"
#define CONTEXT_MT "lualoader.support.context"
#define SLOTS_MT "lualoader.support.slots"
typedef struct context_t context_t;
typedef struct slots_t {
    terraria_item_container_t *native;
    context_t *owner;
    struct slots_t *next;
} slots_t;
struct context_t {
    lua_State *L;
    char mod_id[65];
    slots_t *slots;
    int update_ref;
    bool subscribed;
    bool closed;
};
static context_t *context(lua_State *L) {
    context_t *c = lua_touserdata(L, lua_upvalueindex(1));
    if (!c || c->closed)
        luaL_error(L, "mod support context is closed");
    return c;
}
static terraria_entity_scope_t scope(context_t *c) {
    return (terraria_entity_scope_t){"lzup333.lualoader", c->mod_id};
}
static bool world_linked(void) {
    return terraria_world_entity_put && terraria_world_entity_get && terraria_world_entity_remove &&
           terraria_world_entity_count && terraria_world_entity_at &&
           terraria_world_entity_active && terraria_world_entity_generation;
}
static bool slots_linked(void) {
    return terraria_item_container_create && terraria_item_container_destroy &&
           terraria_item_container_size && terraria_item_container_read &&
           terraria_item_container_swap && terraria_item_container_interact &&
           terraria_item_slot_cursor;
}
static bool recipes_linked(void) {
    return terraria_recipe_query_get && terraria_recipe_query_find &&
           terraria_recipe_query_refresh && terraria_recipe_query_generation;
}
static int integer(lua_State *L, int at, int low, int high) {
    lua_Integer n = luaL_checkinteger(L, at);
    luaL_argcheck(L, n >= low && n <= high, at, "integer out of range");
    return (int)n;
}
static void set_integer(lua_State *L, const char *key, lua_Integer n) {
    lua_pushinteger(L, n);
    lua_setfield(L, -2, key);
}
static void push_item(lua_State *L, const terraria_item_view_t *v) {
    lua_createtable(L, 0, 4);
    set_integer(L, "type", v->type);
    set_integer(L, "stack", v->stack);
    set_integer(L, "max_stack", v->max_stack);
    set_integer(L, "prefix", v->prefix);
}
static int available(lua_State *L) {
    context(L);
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, world_linked());
    lua_setfield(L, -2, "world");
    lua_pushboolean(L, slots_linked());
    lua_setfield(L, -2, "slots");
    lua_pushboolean(L, recipes_linked());
    lua_setfield(L, -2, "recipes");
    return 1;
}
static void close_slots(slots_t *s) {
    if (s->native && terraria_item_container_destroy)
        terraria_item_container_destroy(s->native);
    s->native = NULL;
    if (s->owner) {
        slots_t **p = &s->owner->slots;
        while (*p && *p != s)
            p = &(*p)->next;
        if (*p)
            *p = s->next;
    }
    s->owner = NULL;
    s->next = NULL;
}
static int slots_gc(lua_State *L) {
    slots_t *s = luaL_checkudata(L, 1, SLOTS_MT);
    close_slots(s);
    return 0;
}
static slots_t *slots(lua_State *L, int at) {
    slots_t *s = luaL_checkudata(L, at, SLOTS_MT);
    luaL_argcheck(L, s->native && s->owner && !s->owner->closed, at, "container is closed");
    return s;
}
static int slots_create(lua_State *L) {
    context_t *c = context(L);
    int count = integer(L, 1, 1, 256);
    if (!slots_linked())
        return luaL_error(L, "kernel item slot API unavailable");
    slots_t *s = lua_newuserdatauv(L, sizeof(*s), 1);
    memset(s, 0, sizeof(*s));
    luaL_setmetatable(L, SLOTS_MT);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_setiuservalue(L, -2, 1);
    s->native = terraria_item_container_create((size_t)count);
    if (!s->native)
        return luaL_error(L, "native Item[] container creation failed");
    s->owner = c;
    s->next = c->slots;
    c->slots = s;
    return 1;
}
static int slots_read(lua_State *L) {
    slots_t *s = slots(L, 1);
    int index = integer(L, 2, 1, 256);
    terraria_item_view_t v;
    if (!terraria_item_container_read(s->native, (size_t)index - 1, &v)) {
        lua_pushnil(L);
        return 1;
    }
    push_item(L, &v);
    return 1;
}
static int slots_size(lua_State *L) {
    slots_t *s = slots(L, 1);
    lua_pushinteger(L, (lua_Integer)terraria_item_container_size(s->native));
    return 1;
}
static int slots_swap(lua_State *L) {
    slots_t *a = slots(L, 1);
    int ai = integer(L, 2, 1, 256);
    slots_t *b = slots(L, 3);
    int bi = integer(L, 4, 1, 256);
    luaL_argcheck(L, a->owner == b->owner, 3, "cannot transfer another mod's container");
    lua_pushboolean(
        L, terraria_item_container_swap(a->native, (size_t)ai - 1, b->native, (size_t)bi - 1));
    return 1;
}
static int slots_interact(lua_State *L) {
    slots_t *s = slots(L, 1);
    int index = integer(L, 2, 1, 256), mode = integer(L, 3, 0, 255);
    lua_pushboolean(L, terraria_item_container_interact(s->native, (size_t)index - 1, mode));
    return 1;
}
static int query_cursor(lua_State *L) {
    context(L);
    terraria_item_view_t v;
    if (!slots_linked() || !terraria_item_slot_cursor(&v)) {
        lua_pushnil(L);
        return 1;
    }
    push_item(L, &v);
    return 1;
}
static const char *id_string(lua_State *L, int at) {
    size_t n = 0;
    const char *s = luaL_checklstring(L, at, &n);
    luaL_argcheck(L, n > 0 && n <= 64 && !memchr(s, 0, n), at, "id must be 1-64 bytes without NUL");
    return s;
}
static terraria_entity_key_t entity_key(lua_State *L, context_t *c) {
    return (terraria_entity_key_t){scope(c), id_string(L, 1), integer(L, 2, 0, INT_MAX - 1),
                                   integer(L, 3, 0, INT_MAX - 1)};
}
static int world_active(lua_State *L) {
    context(L);
    lua_pushboolean(L, world_linked() && terraria_world_entity_active());
    return 1;
}
static int world_generation(lua_State *L) {
    context(L);
    lua_pushinteger(L, world_linked() ? (lua_Integer)terraria_world_entity_generation() : 0);
    return 1;
}
static int world_put(lua_State *L) {
    context_t *c = context(L);
    terraria_entity_key_t k = entity_key(L, c);
    size_t n = 0;
    const char *data = luaL_checklstring(L, 4, &n);
    luaL_argcheck(L, n <= TERRARIA_ENTITY_DATA_MAX, 4, "entity payload exceeds 64 KiB");
    lua_pushboolean(L, world_linked() && terraria_world_entity_put(&k, data, n));
    return 1;
}
static int world_get(lua_State *L) {
    context_t *c = context(L);
    terraria_entity_key_t k = entity_key(L, c);
    size_t n = 0;
    uint64_t generation = world_linked() ? terraria_world_entity_generation() : 0;
    if (!world_linked() || !terraria_world_entity_get(&k, NULL, 0, &n)) {
        lua_pushnil(L);
        return 1;
    }
    luaL_Buffer b;
    char *data = luaL_buffinitsize(L, &b, n);
    size_t actual = 0;
    if (!terraria_world_entity_get(&k, data, n, &actual) || actual != n ||
        generation != terraria_world_entity_generation()) {
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&b, n);
    return 1;
}
static int world_remove(lua_State *L) {
    context_t *c = context(L);
    terraria_entity_key_t k = entity_key(L, c);
    lua_pushboolean(L, world_linked() && terraria_world_entity_remove(&k));
    return 1;
}
static int world_list(lua_State *L) {
    context_t *c = context(L);
    terraria_entity_scope_t s = scope(c);
    size_t offset = (size_t)integer(L, 1, 0, 4096);
    int limit = integer(L, 2, 1, 128);
    if (!world_linked())
        return luaL_error(L, "kernel world entity API unavailable");
    uint64_t generation = terraria_world_entity_generation();
    lua_createtable(L, limit, 0);
    int written = 0;
    for (int i = 0; i < limit; ++i) {
        char kind[65];
        int x, y;
        if (!terraria_world_entity_at(&s, offset + (size_t)i, kind, sizeof(kind), &x, &y))
            break;
        lua_createtable(L, 0, 3);
        lua_pushstring(L, kind);
        lua_setfield(L, -2, "kind");
        set_integer(L, "x", x);
        set_integer(L, "y", y);
        lua_rawseti(L, -2, ++written);
    }
    size_t total = terraria_world_entity_count(&s);
    if (generation != terraria_world_entity_generation()) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushliteral(L, "world changed");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)total);
    return 2;
}
static void update(void *user) {
    context_t *c = user;
    if (!c || c->closed || c->update_ref == LUA_NOREF)
        return;
    lua_State *L = c->L;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->update_ref);
    if (lua_pcall(L, 0, 0, 0) !=
        LUA_OK) { /* Keep one error for diagnostics, then stop repeated failing callbacks. */
        lua_setfield(L, LUA_REGISTRYINDEX, "lualoader.support.update_error");
        terraria_world_entity_unsubscribe(c);
        c->subscribed = false;
    }
    lua_settop(L, top);
}
static int world_on_update(lua_State *L) {
    context_t *c = context(L);
    if (!terraria_world_entity_subscribe || !terraria_world_entity_unsubscribe)
        return luaL_error(L, "kernel world update adapter unavailable");
    luaL_argcheck(L, lua_isnil(L, 1) || lua_isfunction(L, 1), 1, "function or nil expected");
    if (c->subscribed) {
        terraria_world_entity_unsubscribe(c);
        c->subscribed = false;
    }
    if (c->update_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, c->update_ref);
        c->update_ref = LUA_NOREF;
    }
    if (lua_isnil(L, 1)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    terraria_entity_scope_t s = scope(c);
    lua_pushvalue(L, 1);
    c->update_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    c->subscribed = terraria_world_entity_subscribe(&s, update, c);
    if (!c->subscribed) {
        luaL_unref(L, LUA_REGISTRYINDEX, c->update_ref);
        c->update_ref = LUA_NOREF;
    }
    lua_pushboolean(L, c->subscribed);
    return 1;
}
static int world_error(lua_State *L) {
    context(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "lualoader.support.update_error");
    return 1;
}
static void push_recipe_item(lua_State *L, const terraria_recipe_item_t *v) {
    lua_createtable(L, 0, 2);
    set_integer(L, "type", v->type);
    set_integer(L, "stack", v->stack);
}
static int recipe_get(lua_State *L) {
    context(L);
    int index = integer(L, 1, 0, 65535);
    terraria_recipe_view_t v;
    if (!recipes_linked() || !terraria_recipe_query_get((size_t)index, &v)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    set_integer(L, "index", v.index);
    push_recipe_item(L, &v.result);
    lua_setfield(L, -2, "result");
    lua_createtable(L, (int)v.material_count, 0);
    for (size_t i = 0; i < v.material_count; ++i) {
        push_recipe_item(L, &v.materials[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "materials");
    lua_createtable(L, (int)v.station_count, 0);
    for (size_t i = 0; i < v.station_count; ++i) {
        lua_pushinteger(L, v.stations[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "stations");
    set_integer(L, "conditions_known", v.conditions_known);
    set_integer(L, "conditions", v.conditions);
    return 1;
}
static int recipe_find(lua_State *L) {
    context(L);
    int type = integer(L, 1, 1, INT_MAX);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    bool usage = lua_toboolean(L, 2);
    int offset = integer(L, 3, 0, 65536);
    int limit = integer(L, 4, 1, 128);
    if (!recipes_linked())
        return luaL_error(L, "kernel recipe query API unavailable");
    int indices[128];
    size_t n = terraria_recipe_query_find(type, usage, (size_t)offset, indices, (size_t)limit),
           written = n > (size_t)offset ? n - (size_t)offset : 0;
    if (written > (size_t)limit)
        written = (size_t)limit;
    lua_createtable(L, (int)written, 0);
    for (size_t i = 0; i < written; ++i) {
        lua_pushinteger(L, indices[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_pushinteger(L, (lua_Integer)n);
    return 2;
}
static int recipe_refresh(lua_State *L) {
    context(L);
    lua_pushboolean(L, recipes_linked() && terraria_recipe_query_refresh());
    return 1;
}
static int recipe_generation(lua_State *L) {
    context(L);
    lua_pushinteger(L, recipes_linked() ? (lua_Integer)terraria_recipe_query_generation() : 0);
    return 1;
}
static void close_context(context_t *c) {
    if (!c || c->closed)
        return;
    c->closed = true;
    if (c->subscribed && terraria_world_entity_unsubscribe)
        terraria_world_entity_unsubscribe(c);
    c->subscribed = false;
    if (c->update_ref != LUA_NOREF)
        luaL_unref(c->L, LUA_REGISTRYINDEX, c->update_ref);
    c->update_ref = LUA_NOREF;
    while (c->slots)
        close_slots(c->slots);
}
static int context_gc(lua_State *L) {
    context_t *c = luaL_checkudata(L, 1, CONTEXT_MT);
    close_context(c);
    return 0;
}
void lualoader_support_close(lua_State *L) {
    if (!L)
        return;
    lua_getfield(L, LUA_REGISTRYINDEX, CONTEXT_KEY);
    context_t *c = luaL_testudata(L, -1, CONTEXT_MT);
    close_context(c);
    lua_pop(L, 1);
}
static void functions(lua_State *L, int ctx, const luaL_Reg *funcs) {
    ctx = lua_absindex(L, ctx);
    for (const luaL_Reg *f = funcs; f->name; ++f) {
        lua_pushvalue(L, ctx);
        lua_pushcclosure(L, f->func, 1);
        lua_setfield(L, -2, f->name);
    }
}
void lualoader_support_register(lua_State *L) {
    int mod = lua_absindex(L, -1);
    lua_getfield(L, mod, "id");
    size_t id_length = 0;
    const char *mod_id = lua_tolstring(L, -1, &id_length);
    if (!mod_id || !id_length || id_length > 64 || memchr(mod_id, 0, id_length)) {
        lua_pop(L, 1);
        return;
    }
    char identity[65];
    memcpy(identity, mod_id, id_length);
    identity[id_length] = 0;
    lua_pop(L, 1);
    mod_id = identity;
    luaL_newmetatable(L, CONTEXT_MT);
    lua_pushcfunction(L, context_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, SLOTS_MT);
    lua_pushcfunction(L, slots_gc);
    lua_setfield(L, -2, "__gc");
    const luaL_Reg methods[] = {{"read", slots_read}, {"size", slots_size},
                                {"swap", slots_swap}, {"interact", slots_interact},
                                {"close", slots_gc},  {NULL, NULL}};
    luaL_setfuncs(L, methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    context_t *c = lua_newuserdatauv(L, sizeof(*c), 0);
    memset(c, 0, sizeof(*c));
    c->L = L;
    c->update_ref = LUA_NOREF;
    strcpy(c->mod_id, mod_id);
    luaL_setmetatable(L, CONTEXT_MT);
    int ctx = lua_absindex(L, -1);
    lua_pushvalue(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, CONTEXT_KEY);
    const luaL_Reg base[] = {{"support_available", available}, {NULL, NULL}};
    lua_pushvalue(L, mod);
    functions(L, ctx, base);
    lua_pop(L, 1);
    const luaL_Reg slot_functions[] = {
        {"create", slots_create}, {"query_cursor", query_cursor}, {NULL, NULL}};
    lua_newtable(L);
    functions(L, ctx, slot_functions);
    lua_setfield(L, mod, "slots");
    const luaL_Reg world_functions[] = {{"active", world_active},
                                        {"generation", world_generation},
                                        {"put", world_put},
                                        {"get", world_get},
                                        {"remove", world_remove},
                                        {"list", world_list},
                                        {"on_update", world_on_update},
                                        {"update_error", world_error},
                                        {NULL, NULL}};
    lua_newtable(L);
    functions(L, ctx, world_functions);
    lua_setfield(L, mod, "world");
    const luaL_Reg query_functions[] = {{"get", recipe_get},
                                        {"find", recipe_find},
                                        {"refresh", recipe_refresh},
                                        {"generation", recipe_generation},
                                        {NULL, NULL}};
    lua_getfield(L, mod, "recipe");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, mod, "recipe");
    }
    functions(L, ctx, query_functions);
    lua_pop(L, 1);
    lua_pop(L, 1);
}
