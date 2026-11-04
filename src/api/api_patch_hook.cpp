#include "api_internal.hpp"

#include <atomic>

namespace lualoader::lua_api {

    // ========================================================================
    // Patchlib: 钩子（Prefix / Postfix）
    // ========================================================================

    struct HookSlot {
        bool used{false};
        uint64_t generation{0};
        lua_State *L{nullptr};
        lua_mod_handle_t *handle{nullptr};
        int prefix_ref{LUA_NOREF};
        int postfix_ref{LUA_NOREF};
        bool copy_handles{false}; ///< true 时把 instance/对象参数/返回值复制为 GC 托管句柄
        bool override_result{false}; ///< true 时允许 postfix 返回值覆盖原方法返回值（默认关闭，保证兼容）
        patch_hook_id_t hook_id{PATCH_HOOK_INVALID_ID};
    };

    static HookSlot g_hook_slots[MAX_HOOK_SLOTS];
    /// 诊断：当前正在派发的 hook 槽位/阶段，供 Lua panic 时定位
    std::atomic<int> g_active_hook_slot{-1};
    std::atomic<int> g_active_hook_phase{0}; // 1=prefix 2=postfix
    static uint64_t g_hook_generation = 0;

    struct HookUserdata {
        int slot;
        uint64_t generation;
    };

    /// 钩子回调：先声明分发函数，再由宏生成固定签名的 C 入口
    static bool hook_prefix_dispatch(int slot, patch_handle_t instance, void **args,
                                     const patch_method_signature_t *sig, void *result);

    static void hook_postfix_dispatch(int slot, patch_handle_t instance, void **args, void *result,
                                      const patch_method_signature_t *sig);

    /// 跳板：按槽位号生成固定签名的 C 入口（用模板展开，数量随 MAX_HOOK_SLOTS 走）
    template<int N>
    static bool prefix_trampoline(patch_handle_t instance, void **args,
                                  const patch_method_signature_t *sig, void *result) {
        return hook_prefix_dispatch(N, instance, args, sig, result);
    }

    template<int N>
    static void postfix_trampoline(patch_handle_t instance, void **args, void *result,
                                   const patch_method_signature_t *sig) {
        hook_postfix_dispatch(N, instance, args, result, sig);
    }

    template<int... Is>
    static std::array<prefix_callback_t, sizeof...(Is)> make_prefix_table(std::integer_sequence<int, Is...>) {
        return {&prefix_trampoline<Is>...};
    }

    template<int... Is>
    static std::array<postfix_callback_t, sizeof...(Is)> make_postfix_table(std::integer_sequence<int, Is...>) {
        return {&postfix_trampoline<Is>...};
    }

    static const std::array<prefix_callback_t, MAX_HOOK_SLOTS> g_prefix_table =
            make_prefix_table(std::make_integer_sequence<int, MAX_HOOK_SLOTS>{});
    static const std::array<postfix_callback_t, MAX_HOOK_SLOTS> g_postfix_table =
            make_postfix_table(std::make_integer_sequence<int, MAX_HOOK_SLOTS>{});

    /// 压入一个参数值；copy_handles 时把托管对象句柄复制为 GC 托管句柄（原生指针只借用，不托管）
    static void push_hook_arg_value(lua_State *L, const patch_type_t type, const void *value, const bool copy_handles) {
        if (copy_handles && type == PATCH_OBJECT && value) {
            auto *ptr = *static_cast<void *const *>(value);
            if (ptr) {
                push_owned_handle(L, copy_handle(ptr));
                return;
            }
        }
        push_patch_value(L, type, value);
    }

    /// 钩子回调参数：instance、参数表 args（数组）、返回值 result
    static void push_hook_args(lua_State *L, patch_handle_t instance, void **args,
                               const patch_method_signature_t *sig, const bool copy_handles) {
        if (!instance) {
            lua_pushnil(L);
        } else if (copy_handles) {
            push_owned_handle(L, copy_handle(instance));
        } else {
            lua_pushlightuserdata(L, instance);
        }
        const int argc = sig ? static_cast<int>(tefstd_vector_size(&sig->arg_types)) : 0;
        if (argc < 0 || argc > 32) {
            LOG_ERROR("[hook] invalid argc={} (slot={}, sig={})", argc,
                      g_active_hook_slot.load(), static_cast<const void *>(sig));
            lua_createtable(L, 0, 0);
            return;
        }
        lua_createtable(L, argc, 0);
        for (int i = 0; i < argc; ++i) {
            const auto type = *static_cast<patch_type_t *>(tefstd_vector_at(&sig->arg_types, i));
            push_hook_arg_value(L, type, args[i], copy_handles);
            lua_rawseti(L, -2, i + 1);
        }
    }

    static bool hook_prefix_dispatch(const int slot, patch_handle_t instance, void **args,
                                     const patch_method_signature_t *sig, void *result) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return false;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used || !s.L || s.prefix_ref == LUA_NOREF) return false;

        lua_State *L = s.L;
        g_active_hook_slot.store(slot, std::memory_order_relaxed);
        g_active_hook_phase.store(1, std::memory_order_relaxed);
        const int base = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.prefix_ref);
        push_hook_args(L, instance, args, sig, s.copy_handles);
        if (result && sig && sig->return_type != PATCH_VOID) {
            push_hook_arg_value(L, sig->return_type, result, s.copy_handles);
        } else {
            lua_pushnil(L);
        }

        // 参数：instance, args, result -> 3 个
        if (pcall_log(L, 3, 2, "prefix hook") != LUA_OK) {
            lua_settop(L, base);
            g_active_hook_slot.store(-1, std::memory_order_relaxed);
            g_active_hook_phase.store(0, std::memory_order_relaxed);
            return false;
        }

        const bool skip = lua_toboolean(L, -2) != 0;
        if (skip && result && sig && sig->return_type != PATCH_VOID && !lua_isnil(L, -1)) {
            if (!try_read_patch_value(L, -1, sig->return_type, result)) {
                LOG_ERROR("[lua] prefix hook returned an invalid result; running original method");
                lua_settop(L, base);
                return false;
            }
        }
        lua_settop(L, base);
        g_active_hook_slot.store(-1, std::memory_order_relaxed);
        g_active_hook_phase.store(0, std::memory_order_relaxed);
        return skip;
    }

    static void hook_postfix_dispatch(const int slot, patch_handle_t instance, void **args, void *result,
                                      const patch_method_signature_t *sig) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used || !s.L || s.postfix_ref == LUA_NOREF) return;

        lua_State *L = s.L;
        g_active_hook_slot.store(slot, std::memory_order_relaxed);
        g_active_hook_phase.store(2, std::memory_order_relaxed);
        const int base = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.postfix_ref);
        push_hook_args(L, instance, args, sig, s.copy_handles);
        if (result && sig && sig->return_type != PATCH_VOID) {
            push_hook_arg_value(L, sig->return_type, result, s.copy_handles);
        } else {
            lua_pushnil(L);
        }

        // 默认忽略 postfix 返回值（兼容旧 Mod）；仅在 opt-in 时用返回值覆盖原方法返回值
        if (s.override_result) {
            if (pcall_log(L, 3, 1, "postfix hook") == LUA_OK) {
                if (result && sig && sig->return_type != PATCH_VOID && !lua_isnil(L, -1)) {
                    if (!try_read_patch_value(L, -1, sig->return_type, result)) {
                        LOG_ERROR("[lua] postfix hook returned an invalid result; keeping original result");
                    }
                }
            }
        } else {
            pcall_log(L, 3, 0, "postfix hook");
        }
        lua_settop(L, base);
        g_active_hook_slot.store(-1, std::memory_order_relaxed);
        g_active_hook_phase.store(0, std::memory_order_relaxed);
    }

    void release_slot(const int slot) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used) return;

        if (s.hook_id != PATCH_HOOK_INVALID_ID) {
            patchlib_uninstall_hook(s.hook_id);
        }
        if (s.L) {
            if (s.prefix_ref != LUA_NOREF) luaL_unref(s.L, LUA_REGISTRYINDEX, s.prefix_ref);
            if (s.postfix_ref != LUA_NOREF) luaL_unref(s.L, LUA_REGISTRYINDEX, s.postfix_ref);
        }
        if (s.handle) {
            auto &slots = s.handle->hook_slots;
            for (auto it = slots.begin(); it != slots.end(); ++it) {
                if (*it == slot) {
                    slots.erase(it);
                    break;
                }
            }
        }
        s = HookSlot{};
    }

    static int alloc_slot() {
        for (int i = 0; i < MAX_HOOK_SLOTS; ++i) {
            if (!g_hook_slots[i].used) {
                g_hook_slots[i].used = true;
                g_hook_slots[i].generation = ++g_hook_generation;
                return i;
            }
        }
        return -1;
    }

    static int ref_function_field(lua_State *L, int table_idx, const char *key) {
        if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
        lua_getfield(L, table_idx, key);
        int ref = LUA_NOREF;
        if (lua_isfunction(L, -1)) {
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
        return ref;
    }

    static int l_patch_install_hook(lua_State *L) {
        const patch_handle_t method = to_handle(L, 1);
        if (!method) return luaL_error(L, "invalid method handle");
        luaL_checktype(L, 2, LUA_TTABLE);

        const int slot = alloc_slot();
        if (slot < 0) return luaL_error(L, "no free hook slots (max %d)", MAX_HOOK_SLOTS);

        HookSlot &s = g_hook_slots[slot];
        s.L = L;
        s.handle = get_handle(L);
        s.prefix_ref = ref_function_field(L, 2, "prefix");
        s.postfix_ref = ref_function_field(L, 2, "postfix");
        lua_getfield(L, 2, "copy");
        s.copy_handles = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        // 显式 opt-in 才允许 postfix 返回值覆盖原方法返回值（默认关闭，保证旧 Mod 兼容）
        lua_getfield(L, 2, "override_result");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, 2, "result");
        }
        s.override_result = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        if (s.prefix_ref == LUA_NOREF && s.postfix_ref == LUA_NOREF) {
            s = HookSlot{};
            return luaL_error(L, "hook must define a 'prefix' and/or 'postfix' function");
        }

        const prefix_callback_t prefix = s.prefix_ref != LUA_NOREF ? g_prefix_table[slot] : nullptr;
        const postfix_callback_t postfix = s.postfix_ref != LUA_NOREF ? g_postfix_table[slot] : nullptr;
        s.hook_id = patchlib_install_prepost_hook(method, prefix, postfix);

        if (s.hook_id == PATCH_HOOK_INVALID_ID) {
            if (s.prefix_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.prefix_ref);
            if (s.postfix_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.postfix_ref);
            s = HookSlot{};
            return luaL_error(L, "failed to install hook");
        }

        if (s.handle) s.handle->hook_slots.push_back(slot);

        auto *ud = static_cast<HookUserdata *>(lua_newuserdatauv(L, sizeof(HookUserdata), 0));
        ud->slot = slot;
        ud->generation = s.generation;
        luaL_getmetatable(L, HOOK_METATABLE);
        lua_setmetatable(L, -2);
        return 1;
    }

    int l_hook_remove(lua_State *L) {
        auto *ud = static_cast<HookUserdata *>(luaL_checkudata(L, 1, HOOK_METATABLE));
        if (ud->slot >= 0) {
            // A removed hook's Lua userdata may survive after its slot is reused.
            if (ud->slot < MAX_HOOK_SLOTS &&
                g_hook_slots[ud->slot].generation == ud->generation) {
                release_slot(ud->slot);
            }
            ud->slot = -1;
        }
        return 0;
    }


void add_patch_hook(lua_State *L) {
    static const luaL_Reg fns[] = {
            {"install_hook", l_patch_install_hook},
            {nullptr, nullptr},
    };
    luaL_setfuncs(L, fns, 0);
}

} // namespace lualoader::lua_api
