/*******************************************************************************
 * LuaLoader - gui (native Terraria GUI)
 * Copyright (C) 2026 lzup333
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

// ============================================================================
// 原生 GUI（不再依赖 Dear ImGui）
//
//   PC 端   : XNA / FNA        —— Terraria.Utils.DrawBorderString / DrawInvBG
//   PE 端   : Unity + XNA 兼容 —— 同一套 SpriteBatch / Utils 接口
//
//  说明：
//   - 保留原有 Lua 接口 mod.gui.*（text/button/checkbox/slider/input_text/...）
//   - 所有绘制都走游戏自己的 SpriteBatch/字体，不再叠加独立渲染后端
//   - 每帧在 Terraria.Main.Draw(GameTime) 之后绘制，鼠标使用 Main.mouseX/Y 等
//     游戏自己的输入状态，因此在各平台表现一致。
// ============================================================================

#include "gui.hpp"
#include "gui_images.hpp"
#include "gui_viewport.hpp"
#include <deque>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "json.hpp"
#include <cmath>
#include "gui_icon_gesture.hpp"
#include "gui_click_state.hpp"
#include "gui_keyboard_bounds.hpp"
#include "gui_ime.hpp"
#include "gui_keyboard.hpp"
#include "pinyin.hpp"

#include <cctype>
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "logger.hpp"
#include "core.hpp"
#include "lua_engine.hpp"

#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/string.h"
#include "tefkernel/patchlib/type.h"

namespace lualoader::gui {

    namespace {
#include "gui/gui_types.inc"
#include "gui/gui_window.inc"
#include "gui/gui_lua.inc"
#include "gui/gui_hook.inc"

    } // namespace

    void configure_storage(const std::string &root){load_layout(root);}

    bool installed() { return g_installed; }

    void install() {
        if (g_installed) return;

        if (env_flag("LUALOADER_GUI_DISABLE")) {
            LOG_INFO("[gui] native GUI disabled by LUALOADER_GUI_DISABLE");
            return;
        }

        const patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
        if (!main_type) {
            LOG_WARN("[gui] Terraria.Main not found, native GUI disabled");
            return;
        }

        patch_handle_t render_target = patchlib_type_get_method_by_param_count(main_type, "Draw", 1);
        if (!render_target || !patchlib_is_valid(render_target)) {
            patchlib_free(render_target);
            render_target = patchlib_type_get_method(main_type, "Draw");
        }
        if (!render_target || !patchlib_is_valid(render_target)) {
            LOG_WARN("[gui] render hook target not found, native GUI disabled");
            patchlib_free(render_target);
            patchlib_free(main_type);
            return;
        }
        g_draw_hook = patchlib_install_prepost_hook(render_target, nullptr, frame_postfix);
        // 安装完成后 hook 自带方法指针，不再需要句柄。
        patchlib_free(render_target);
        if (g_draw_hook == PATCH_HOOK_INVALID_ID) {
            LOG_ERROR("[gui] failed to install render hook");
            patchlib_free(main_type);
            return;
        }

        // 输入屏蔽：在 Main.Update 之前判断是否悬停 GUI
        patch_handle_t update = patchlib_type_get_method_by_param_count(main_type, "Update", 1);
        if (!update || !patchlib_is_valid(update)) {
            patchlib_free(update);
            update = patchlib_type_get_method_by_param_count(main_type, "DoUpdate", 1);
        }
        if (update && patchlib_is_valid(update)) {
            g_update_hook = patchlib_install_prepost_hook(update, update_prefix, nullptr);
            LOG_INFO("[gui] hooked Main.Update for input suppression (id={})", static_cast<int>(g_update_hook));
        } else {
            LOG_WARN("[gui] Main.Update not found; GUI will not suppress game mouse input");
        }
        patchlib_free(update);
        patchlib_free(main_type);

        // Block mobile controller input at dispatch, before button callbacks run.
#if defined(__ANDROID__)
        const auto layer_type=patchlib_type_get_type("", "CursorInputLayer");
        const auto layer_update=layer_type ? patchlib_type_get_method_by_param_count(layer_type,"UpdateCursor",1) : nullptr;
        if(layer_update && patchlib_is_valid(layer_update))
            g_layer_hook=patchlib_install_prepost_hook(layer_update,cursor_layer_prefix,nullptr);
        patchlib_free(layer_update);
        patchlib_free(layer_type);
        const auto cm_type=patchlib_type_get_type("", "CursorManager");
        const auto list_update=cm_type ? patchlib_type_get_method_by_param_count(cm_type,"UpdateCursorList",0) : nullptr;
        if(list_update && patchlib_is_valid(list_update))
            g_cursor_list_hook=patchlib_install_prepost_hook(list_update,nullptr,cursor_list_postfix);
        patchlib_free(list_update);
        patchlib_free(cm_type);
        LOG_INFO("[gui] touch capture layer={} cursor-list={}",static_cast<int>(g_layer_hook),static_cast<int>(g_cursor_list_hook));
#endif
        // 预解析绘制接口
        resolve_gfx();
        g_installed = true;
        LOG_INFO("[gui] native GUI enabled (render hook id={})", static_cast<int>(g_draw_hook));
    }

    void shutdown() {
        if (g_draw_hook != PATCH_HOOK_INVALID_ID) {
            patchlib_uninstall_hook(g_draw_hook);
            g_draw_hook = PATCH_HOOK_INVALID_ID;
        }
        if (g_update_hook != PATCH_HOOK_INVALID_ID) {
            patchlib_uninstall_hook(g_update_hook);
            g_update_hook = PATCH_HOOK_INVALID_ID;
        }
        if (g_pi_hook != PATCH_HOOK_INVALID_ID) {
            patchlib_uninstall_hook(g_pi_hook);
            g_pi_hook = PATCH_HOOK_INVALID_ID;
        }
        if (g_cursor_hook != PATCH_HOOK_INVALID_ID) {
            patchlib_uninstall_hook(g_cursor_hook);
            g_cursor_hook = PATCH_HOOK_INVALID_ID;
        }
#if defined(__ANDROID__)
        if(g_layer_hook!=PATCH_HOOK_INVALID_ID)patchlib_uninstall_hook(g_layer_hook);
        if(g_cursor_list_hook!=PATCH_HOOK_INVALID_ID)patchlib_uninstall_hook(g_cursor_list_hook);
        g_layer_hook=g_cursor_list_hook=PATCH_HOOK_INVALID_ID;
        g_captured_touches.clear();
#endif
        clear_text_cache();images::clear();
        // GUI 自己的反射句柄与输入法/键盘缓存也必须释放，否则每次重新加载都会泄漏。
        release_gfx();
        ime::shutdown();
        kbd::shutdown();
        g_image_release_queue.clear();
        g_scrolls.clear();g_scroll_stack.clear();g_active_scroll.clear();g_viewport={};
        g_style_window=nullptr;g_ctx=nullptr;g_sb=nullptr;g_recording=false;g_in_render=false;
        save_layout();g_folders.clear();g_open_folder.clear();g_member_gestures.clear();g_position_applied.clear();
        g_windows.clear();g_headers.clear();g_edit=EditState{};g_cmds.clear();g_next_window_order=0;
        g_render_owner.clear();g_input_owner.clear();g_pointer_owner.clear();g_current_mod.clear();
        g_drag_id.clear();g_slider_active.clear();g_click_state.reset();
        g_mouse_down=g_prev_mouse_down=g_mouse_pressed=g_mouse_released=g_hover_gui=false;
        g_mouse_x=g_mouse_y=-FLT_MAX;
#if defined(__ANDROID__)
        g_active_cursor_valid=false;
#endif
        g_gfx=Gfx{};
        g_installed = false;
    }

    void register_api(lua_State *L, lua_mod_handle_t *) {
        // 在 mod 表（栈顶）上挂 mod.gui
        lua_newtable(L);
        luaL_setfuncs(L, gui_funcs, 0);
        lua_pushinteger(L, 2);
        lua_setfield(L, -2, "image_api_version");
        lua_setfield(L, -2, "gui");
    }

} // namespace lualoader::gui
