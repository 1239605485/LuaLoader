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

#include <cctype>
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

        // ====================================================================
        // 与托管结构体对应的 POD 镜像
        // ====================================================================

        struct Vec2 {
            float x, y;
        };

        // Microsoft.Xna.Framework.Rectangle
        struct Recti {
            int x, y, w, h;
        };

        // Microsoft.Xna.Framework.Color 的内存布局因平台而异：
        //   - PE(Unity) 打包为 4 字节 A,B,G,R
        //   - PC(XNA/FNA) 为 R,G,B,A + packedValue（8 字节）
#if defined(__ANDROID__)
        struct Color {
            unsigned char a, b, g, r;
        };
#else
        struct Color {
            unsigned char r, g, b, a;
            unsigned int packed;
        };
#endif

        void make_color(Color &c, int r, int g, int b, int a) {
#if defined(__ANDROID__)
            c.a = static_cast<unsigned char>(a);
            c.b = static_cast<unsigned char>(b);
            c.g = static_cast<unsigned char>(g);
            c.r = static_cast<unsigned char>(r);
#else
            c.r = static_cast<unsigned char>(r);
            c.g = static_cast<unsigned char>(g);
            c.b = static_cast<unsigned char>(b);
            c.a = static_cast<unsigned char>(a);
            c.packed = static_cast<unsigned>(r) | (static_cast<unsigned>(g) << 8) |
                       (static_cast<unsigned>(b) << 16) | (static_cast<unsigned>(a) << 24);
#endif
        }

        // ====================================================================
        // 反射辅助
        // ====================================================================

        /// 一个字段或（属性）访问器。
        struct Accessor {
            patch_handle_t field{nullptr};
            patch_handle_t getter{nullptr};
            patch_handle_t setter{nullptr};

            [[nodiscard]] bool valid() const { return field != nullptr || getter != nullptr; }
        };

        Accessor resolve_accessor(const patch_handle_t type, const char *name) {
            Accessor a;
            if (!type || !name) return a;
            a.field = patchlib_type_get_field(type, name);
            if (!a.field || !patchlib_is_valid(a.field)) {
                a.field = nullptr;
                const patch_handle_t prop = patchlib_type_get_property(type, name);
                if (prop && patchlib_is_valid(prop)) {
                    a.getter = patchlib_property_get_get_method(prop);
                    a.setter = patchlib_property_get_set_method(prop);
                }
            }
            return a;
        }

        int read_int(const Accessor &a, const int fallback = 0) {
            if (a.field) {
#if defined(__ANDROID__)
                void *ptr = patchlib_field_get_pointer(a.field, nullptr);
                if (ptr) return *static_cast<int *>(ptr);
#else
                int v = 0;
                patchlib_field_get_value(a.field, nullptr, &v);
                return v;
#endif
            }
            if (a.getter) {
                int v = 0;
                patchlib_method_invoke_args(a.getter, nullptr, &v, nullptr);
                return v;
            }
            return fallback;
        }

        bool read_bool(const Accessor &a, const bool fallback = false) {
            if (a.field) {
#if defined(__ANDROID__)
                void *ptr = patchlib_field_get_pointer(a.field, nullptr);
                if (ptr) return *static_cast<unsigned char *>(ptr) != 0;
#else
                unsigned char v = 0;
                patchlib_field_get_value(a.field, nullptr, &v);
                return v != 0;
#endif
            }
            if (a.getter) {
                bool v = false;
                patchlib_method_invoke_args(a.getter, nullptr, &v, nullptr);
                return v;
            }
            return fallback;
        }

        void write_bool(const Accessor &a, const bool value) {
            const unsigned char b = value ? 1 : 0;
            if (a.field) {
#if defined(__ANDROID__)
                void *ptr = patchlib_field_get_pointer(a.field, nullptr);
                if (ptr) *static_cast<unsigned char *>(ptr) = b;
#else
                patchlib_field_set_value(a.field, nullptr, const_cast<unsigned char *>(&b));
#endif
            } else if (a.setter) {
                void *args[1] = {const_cast<unsigned char *>(&b)};
                patchlib_method_invoke_args(a.setter, nullptr, nullptr, args);
            }
        }

        patch_handle_t read_static_object(const patch_handle_t field) {
            if (!field) return nullptr;
#if defined(__ANDROID__)
            void *ptr = patchlib_field_get_pointer(field, nullptr);
            return ptr ? *static_cast<patch_handle_t *>(ptr) : nullptr;
#else
            patch_handle_t v = nullptr;
            patchlib_field_get_value(field, nullptr, &v);
            return v;
#endif
        }

        patch_handle_t get_method(const patch_handle_t type, const char *name, const int argc) {
            if (!type || !name) return nullptr;
            patch_handle_t m = patchlib_type_get_method_by_param_count(type, name, argc);
            if (!m || !patchlib_is_valid(m)) m = nullptr;
            return m;
        }

        // ====================================================================
        // 游戏绘制接口缓存
        // ====================================================================

        struct Gfx {
            bool ready{false};
            bool usable{false};

            patch_handle_t t_main{nullptr};
            patch_handle_t f_sprite_batch{nullptr};

            patch_handle_t m_sb_begin{nullptr};
            patch_handle_t m_sb_end{nullptr};
            int sb_begin_args{0};

            patch_handle_t m_draw_inv_bg{nullptr};     // Utils.DrawInvBG(SpriteBatch,Rectangle,Color)
            patch_handle_t m_draw_border_string{nullptr}; // Utils.DrawBorderString(...)

            Accessor a_screen_w, a_screen_h;
            Accessor a_mouse_x, a_mouse_y;
            Accessor a_mouse_left, a_mouse_left_release, a_mouse_right;
            Accessor a_show_splash;
        };

        Gfx g_gfx;
        patch_handle_t g_sb{nullptr}; // 当前帧的 spriteBatch

        /// 读取布尔环境变量（"0"/空 视为 false）
        bool env_flag(const char *name) {
            const char *v = std::getenv(name);
            return v && v[0] != '\0' && v[0] != '0';
        }

        void resolve_gfx() {
            if (g_gfx.ready) return;
            g_gfx.ready = true;

            g_gfx.t_main = patchlib_type_get_type("Terraria", "Main");
            if (g_gfx.t_main) {
                g_gfx.f_sprite_batch = patchlib_type_get_field(g_gfx.t_main, "spriteBatch");
                if (g_gfx.f_sprite_batch && !patchlib_is_valid(g_gfx.f_sprite_batch))
                    g_gfx.f_sprite_batch = nullptr;

                g_gfx.a_screen_w = resolve_accessor(g_gfx.t_main, "screenWidth");
                g_gfx.a_screen_h = resolve_accessor(g_gfx.t_main, "screenHeight");
                g_gfx.a_mouse_x = resolve_accessor(g_gfx.t_main, "mouseX");
                g_gfx.a_mouse_y = resolve_accessor(g_gfx.t_main, "mouseY");
                g_gfx.a_mouse_left = resolve_accessor(g_gfx.t_main, "mouseLeft");
                g_gfx.a_mouse_left_release = resolve_accessor(g_gfx.t_main, "mouseLeftRelease");
                g_gfx.a_mouse_right = resolve_accessor(g_gfx.t_main, "mouseRight");
                g_gfx.a_show_splash = resolve_accessor(g_gfx.t_main, "showSplash");
            }

            const patch_handle_t t_sb =
                    patchlib_type_get_type("Microsoft.Xna.Framework.Graphics", "SpriteBatch");
            if (t_sb) {
                g_gfx.m_sb_begin = get_method(t_sb, "Begin", 0);
                if (g_gfx.m_sb_begin) {
                    g_gfx.sb_begin_args = 0;
                } else {
                    g_gfx.m_sb_begin = get_method(t_sb, "Begin", 2);
                    g_gfx.sb_begin_args = 2;
                    if (!g_gfx.m_sb_begin) {
                        g_gfx.m_sb_begin = get_method(t_sb, "Begin", 8);
                        g_gfx.sb_begin_args = 8;
                    }
                }
                g_gfx.m_sb_end = get_method(t_sb, "End", 0);
            }

            const patch_handle_t t_utils = patchlib_type_get_type("Terraria", "Utils");
            if (t_utils) {
                g_gfx.m_draw_inv_bg = get_method(t_utils, "DrawInvBG", 3);
                g_gfx.m_draw_border_string = get_method(t_utils, "DrawBorderString", 8);
            }

            g_gfx.usable = g_gfx.f_sprite_batch && g_gfx.m_sb_begin && g_gfx.m_sb_end &&
                           g_gfx.m_draw_inv_bg && g_gfx.m_draw_border_string;

            LOG_INFO("[gui] native GUI backend: spriteBatch={} begin={} end={} invBG={} text={} showSplash={} usable={}",
                     reinterpret_cast<void *>(g_gfx.f_sprite_batch),
                     reinterpret_cast<void *>(g_gfx.m_sb_begin),
                     reinterpret_cast<void *>(g_gfx.m_sb_end),
                     reinterpret_cast<void *>(g_gfx.m_draw_inv_bg),
                     reinterpret_cast<void *>(g_gfx.m_draw_border_string),
                     g_gfx.a_show_splash.valid(),
                     g_gfx.usable);
        }

        // ====================================================================
        // 绘制原语
        // ====================================================================

        // --------------------------------------------------------------------
        // 绘制指令队列：mod.gui.* 只记录指令，等 on_gui 返回后在 C++ 里统一执行。
        // 原因：托管绘制方法（Utils.*）在 Android 上抛出的异常是 il2cpp 的 C++
        // 异常；若在 Lua 回调内直接调用，异常会穿过 Lua VM 的 setjmp 帧导致
        // std::terminate/abort。放到 Lua 之外执行即可规避。
        // --------------------------------------------------------------------
        struct DrawCmd {
            enum Kind { RECT, TEXT } kind;
            float x, y, w, h; // RECT 用 w/h；TEXT 用 scale
            Color col;
            std::string text;
            float scale;
        };

        std::vector<DrawCmd> g_cmds;
        bool g_recording = false;

#if defined(__ANDROID__)
        // Android 兜底：内核 patchlib 的按值结构体 FFI 编组在含静态字段的结构体
        // （Vector2/Color/Rectangle 都有）上会生成错误的 ffi_type，导致托管方法
        // 收到错位参数并抛异常（→ std::terminate → SIGABRT）。
        // 这里直接用原生函数指针按正确的 C ABI 调用，绕开 patch_type_to_ffi_type。
        // 详见 TEFKernel/documents/patchlib-ffi-struct-bug.md。
        using FnDrawInvBG = void (*)(void *sb, Recti r, Color c);
        using FnDrawString = Vec2 (*)(void *sb, void *text, Vec2 pos, Color color,
                                      float scale, float ax, float ay, int32_t max_chars);
#endif

        void exec_rect(const float x, const float y, const float w, const float h, const Color &c) {
            if (!g_gfx.m_draw_inv_bg || !g_sb || w <= 0 || h <= 0) return;
            Recti r{static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)};
            Color col = c;
#if defined(__ANDROID__)
            auto fn = reinterpret_cast<FnDrawInvBG>(patchlib_method_get_pointer(g_gfx.m_draw_inv_bg));
            if (fn) fn(g_sb, r, col);
#else
            void *args[3] = {&g_sb, &r, &col};
            patchlib_method_invoke_args(g_gfx.m_draw_inv_bg, nullptr, nullptr, args);
#endif
        }

        void exec_string(const float x, const float y, const char *text, const Color &c, const float scale) {
            if (!g_gfx.m_draw_border_string || !g_sb || !text || !*text) return;
            patch_handle_t str = patchlib_string_create(text);
            if (!str) return;
            Vec2 pos{x, y};
            Color col = c;
#if defined(__ANDROID__)
            auto fn = reinterpret_cast<FnDrawString>(patchlib_method_get_pointer(g_gfx.m_draw_border_string));
            if (fn) fn(g_sb, str, pos, col, scale, 0.0f, 0.0f, -1);
#else
            float ax = 0.0f, ay = 0.0f;
            int max_chars = -1;
            float fscale = scale;
            void *args[8] = {&g_sb, &str, &pos, &col, &fscale, &ax, &ay, &max_chars};
            patchlib_method_invoke_args(g_gfx.m_draw_border_string, nullptr, nullptr, args);
#endif
            patchlib_free(str);
        }

        /// 执行当前记录的所有指令（必须在 Lua 回调之外调用）
        void flush_cmds() {
            for (const DrawCmd &cmd: g_cmds) {
                if (cmd.kind == DrawCmd::RECT) {
                    exec_rect(cmd.x, cmd.y, cmd.w, cmd.h, cmd.col);
                } else {
                    exec_string(cmd.x, cmd.y, cmd.text.c_str(), cmd.col, cmd.scale);
                }
            }
            g_cmds.clear();
        }

        void draw_rect(const float x, const float y, const float w, const float h, const Color &c) {
            if (g_recording) {
                g_cmds.push_back(DrawCmd{DrawCmd::RECT, x, y, w, h, c, {}, 0.0f});
                return;
            }
            exec_rect(x, y, w, h, c);
        }

        void draw_string(const float x, const float y, const char *text, const Color &c,
                         const float scale = 1.0f) {
            if (!text || !*text) return;
            if (g_recording) {
                g_cmds.push_back(DrawCmd{DrawCmd::TEXT, x, y, 0.0f, 0.0f, c, text, scale});
                return;
            }
            exec_string(x, y, text, c, scale);
        }

        void sb_begin() {
            if (!g_gfx.m_sb_begin || !g_sb) return;
            if (g_gfx.sb_begin_args == 0) {
                patchlib_method_invoke_args(g_gfx.m_sb_begin, g_sb, nullptr, nullptr);
            } else if (g_gfx.sb_begin_args == 2) {
                int sort_mode = 0; // SpriteSortMode.Deferred
                unsigned char deferred = 1;
                void *args[2] = {&sort_mode, &deferred};
                patchlib_method_invoke_args(g_gfx.m_sb_begin, g_sb, nullptr, args);
            } else {
                int sort_mode = 0;
                void *null_obj = nullptr;
                unsigned char deferred = 1;
                void *args[8] = {&sort_mode, &null_obj, &null_obj, &null_obj,
                                 &null_obj, &null_obj, &null_obj, &deferred};
                patchlib_method_invoke_args(g_gfx.m_sb_begin, g_sb, nullptr, args);
            }
        }

        void sb_end() {
            if (!g_gfx.m_sb_end || !g_sb) return;
            patchlib_method_invoke_args(g_gfx.m_sb_end, g_sb, nullptr, nullptr);
        }

        // ====================================================================
        // 屏幕尺寸 / UI 缩放 / 输入
        // ====================================================================

        int g_screen_w = 1280;
        int g_screen_h = 720;
        float g_scale = 1.0f;

        float g_mouse_x = -FLT_MAX;
        float g_mouse_y = -FLT_MAX;
        bool g_mouse_down = false;
        bool g_mouse_pressed = false;
        bool g_mouse_released = false;
        bool g_prev_mouse_down = false;
        bool g_hover_gui = false;

        void update_screen_size() {
            const int w = read_int(g_gfx.a_screen_w, 0);
            const int h = read_int(g_gfx.a_screen_h, 0);
            if (w > 0) g_screen_w = w;
            if (h > 0) g_screen_h = h;
            int mn = g_screen_w < g_screen_h ? g_screen_w : g_screen_h;
            if (mn <= 0) mn = 720;
            float s = static_cast<float>(mn) / 720.0f;
            if (s < 1.0f) s = 1.0f;
            if (s > 2.5f) s = 2.5f;
            g_scale = s;
        }

        void update_input() {
            g_mouse_x = static_cast<float>(read_int(g_gfx.a_mouse_x, 0));
            g_mouse_y = static_cast<float>(read_int(g_gfx.a_mouse_y, 0));
            g_mouse_down = read_bool(g_gfx.a_mouse_left, false);
            g_mouse_pressed = g_mouse_down && !g_prev_mouse_down;
            g_mouse_released = !g_mouse_down && g_prev_mouse_down;
            g_prev_mouse_down = g_mouse_down;
        }

        bool mouse_in(const float x, const float y, const float w, const float h) {
            if (g_mouse_x == -FLT_MAX) return false;
            return g_mouse_x >= x && g_mouse_x <= x + w && g_mouse_y >= y && g_mouse_y <= y + h;
        }

        // ====================================================================
        // 文本宽度估算（MouseText 字体约 20px）
        // ====================================================================

        /// 前进一个 UTF-8 字符，返回该字符占用字节数。
        size_t utf8_next(const char *s) {
            const unsigned char c = static_cast<unsigned char>(s[0]);
            size_t len = 1;
            if ((c & 0x80) == 0x00) len = 1;
            else if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            for (size_t i = 1; i < len; ++i) {
                if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return i;
            }
            return len;
        }

        float text_width(const char *s, const float scale = 1.0f) {
            if (!s) return 0.0f;
            float w = 0.0f;
            for (const char *p = s; *p;) {
                const size_t n = utf8_next(p);
                w += (n == 1) ? 10.0f : 20.0f; // ASCII 半角 / 多字节全角
                p += n;
            }
            return w * g_scale * scale;
        }

        // ====================================================================
        // 颜色表（Terraria 风格深色面板）
        // ====================================================================

        const Color kText = [] {
            Color c;
            make_color(c, 230, 230, 230, 255);
            return c;
        }();
        const Color kTextDim = [] {
            Color c;
            make_color(c, 170, 175, 190, 255);
            return c;
        }();
        const Color kPanel = [] {
            Color c;
            make_color(c, 23, 25, 81, 235);
            return c;
        }();
        const Color kTitle = [] {
            Color c;
            make_color(c, 50, 60, 110, 255);
            return c;
        }();
        const Color kBtn = [] {
            Color c;
            make_color(c, 45, 55, 95, 255);
            return c;
        }();
        const Color kBtnHover = [] {
            Color c;
            make_color(c, 70, 90, 150, 255);
            return c;
        }();
        const Color kBtnActive = [] {
            Color c;
            make_color(c, 100, 130, 200, 255);
            return c;
        }();
        const Color kTrack = [] {
            Color c;
            make_color(c, 15, 15, 30, 255);
            return c;
        }();
        const Color kFill = [] {
            Color c;
            make_color(c, 120, 160, 230, 255);
            return c;
        }();
        const Color kWhite = [] {
            Color c;
            make_color(c, 255, 255, 255, 255);
            return c;
        }();
        const Color kBorder = [] {
            Color c;
            make_color(c, 12, 14, 34, 255);
            return c;
        }();
        const Color kTrackEdge = [] {
            Color c;
            make_color(c, 6, 8, 20, 255);
            return c;
        }();

        // ====================================================================
        // 布局
        // ====================================================================

        constexpr float PAD = 12.0f;
        constexpr float TITLE_H = 32.0f;
        constexpr float ITEM_GAP = 12.0f;

        float line_height() { return 28.0f * g_scale; }
        float button_height() { return 32.0f * g_scale; }
        float item_gap() { return ITEM_GAP * g_scale; }
        float pad() { return PAD * g_scale; }
        float title_height() { return TITLE_H * g_scale; }

        struct Ctx {
            float ox{0}, oy{0};   // 内容原点
            float cw{0};          // 内容宽度
            float cx{0}, cy{0};   // 当前光标（相对内容原点）
            float prev_h{0};
            bool same_line{false};
        };

        Ctx *g_ctx = nullptr;
        std::string g_current_mod;

        void place(float &x, float &y, const float w, const float h) {
            if (!g_ctx->same_line) {
                g_ctx->cx = 0.0f;
                g_ctx->cy += g_ctx->prev_h;
                if (g_ctx->prev_h > 0.0f) g_ctx->cy += 4.0f * g_scale; // 行间留白
                g_ctx->prev_h = 0.0f;
            }
            g_ctx->same_line = false;
            x = g_ctx->ox + g_ctx->cx;
            y = g_ctx->oy + g_ctx->cy;
            g_ctx->cx += w + item_gap();
            if (h > g_ctx->prev_h) g_ctx->prev_h = h;
        }

        void advance_line() {
            g_ctx->cx = 0.0f;
            g_ctx->cy += g_ctx->prev_h;
            if (g_ctx->prev_h > 0.0f) g_ctx->cy += 4.0f * g_scale;
            g_ctx->prev_h = 0.0f;
            g_ctx->same_line = false;
        }

        // ====================================================================
        // 每个 Mod 的窗口状态
        // ====================================================================

        struct WindowState {
            float x{40}, y{40};
            float w{380}, h{160};
            bool visible{true};   ///< 是否显示（Mod 可控制出现/消失）
            int order{0};
        };

        std::unordered_map<std::string, WindowState> g_windows;

        std::string g_drag_id;
        float g_drag_dx = 0.0f, g_drag_dy = 0.0f;

        std::string g_slider_active;
        std::unordered_map<std::string, bool> g_headers;

        // 文本输入状态
        struct EditState {
            bool active{false};
            std::string owner;
            std::string buf;
            bool shift{false};
        };
        EditState g_edit;

        void draw_virtual_keyboard();

        // ====================================================================
        // 绘制单个 Mod 的窗口
        // ====================================================================

        void draw_mod_gui(lua_mod_handle_t *h) {
            if (!h || h->gui_ref == LUA_NOREF) return;

            WindowState &ws = g_windows[h->mod_id];

            // 隐藏时：仍然回调 on_gui（Mod 可在此把自己设回显示），但不绘制、不响应输入
            if (!ws.visible) {
                g_ctx = nullptr;
                g_current_mod = h->mod_id;
                g_recording = true;
                g_cmds.clear();
                lua_engine::call_on_gui(h);
                g_recording = false;
                g_current_mod.clear();
                g_cmds.clear();
                return;
            }

            const float p = pad();
            const float th = title_height();

            // 外边框 + 背景 + 标题栏（用上一帧的尺寸，首帧用默认值）
            draw_rect(ws.x - 2.0f, ws.y - 2.0f, ws.w + 4.0f, ws.h + 4.0f, kBorder);
            draw_rect(ws.x, ws.y, ws.w, ws.h, kPanel);
            draw_rect(ws.x, ws.y, ws.w, th, kTitle);
            draw_rect(ws.x, ws.y + th - 1.0f, ws.w, 1.0f, kBorder); // 标题栏分隔线
            draw_string(ws.x + p, ws.y + (th - line_height()) * 0.5f, h->mod_id.c_str(), kWhite, 1.0f);

            const bool title_hovered = mouse_in(ws.x, ws.y, ws.w, th);
            if (title_hovered) g_hover_gui = true;
            if (title_hovered && g_mouse_pressed && g_drag_id.empty()) {
                g_drag_id = h->mod_id;
                g_drag_dx = g_mouse_x - ws.x;
                g_drag_dy = g_mouse_y - ws.y;
            }
            if (!g_drag_id.empty() && g_drag_id == h->mod_id && g_mouse_down) {
                ws.x = g_mouse_x - g_drag_dx;
                ws.y = g_mouse_y - g_drag_dy;
            }

            // 内容：先记录指令（在 Lua 回调内不能直接调托管绘制），
            // 回调返回后再离开 Lua VM 统一执行。
            Ctx ctx;
            ctx.ox = ws.x + p;
            ctx.oy = ws.y + th + p * 0.5f;
            ctx.cw = ws.w - p * 2.0f;
            g_ctx = &ctx;
            g_current_mod = h->mod_id;
            g_cmds.clear();
            g_recording = true;
            lua_engine::call_on_gui(h);
            g_recording = false;
            g_current_mod.clear();
            g_ctx = nullptr;
            flush_cmds();

            const float content_h = ctx.cy + ctx.prev_h;
            ws.h = th + p + content_h + p;
        }

        // ====================================================================
        // 虚拟键盘（文本输入）
        // ====================================================================

        const char *const kRow1 = "1234567890";
        const char *const kRow2 = "qwertyuiop";
        const char *const kRow3 = "asdfghjkl";
        const char *const kRow4 = "zxcvbnm";

        void vkb_append(const char c) {
            g_edit.buf.push_back(c);
        }

        /// 画一个方块按键（带边框 + 居中文字）
        void draw_key_box(const float x, const float y, const float w, const float h, const char *label,
                          const bool hovered) {
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1, hovered ? kBtnHover : kBtn);
            draw_string(x + (w - text_width(label)) * 0.5f, y + (h - line_height()) * 0.5f, label,
                        kWhite, 1.0f);
        }

        void draw_key(const float x, const float y, const float w, const float h, const char *label,
                      const char out, bool &consumed) {
            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            if (hovered && g_mouse_released) {
                vkb_append(out);
                consumed = true;
            }
            draw_key_box(x, y, w, h, label, hovered);
        }

        void draw_virtual_keyboard() {
            const float key = 34.0f * g_scale;
            const float wide = 56.0f * g_scale;
            const float gap = 4.0f * g_scale;
            const float pd = 10.0f * g_scale;

            // 各行内容宽度
            const float w_num = 10.0f * key + 9.0f * gap;
            const float w_qwe = 10.0f * key + 9.0f * gap;
            const float w_asd = 9.0f * key + 8.0f * gap;
            const float w_fun = wide + gap + 7.0f * key + 6.0f * gap + gap + wide;
            const float w_spc = 6.0f * key + gap + 3.0f * key;
            float inner = w_num;
            if (w_qwe > inner) inner = w_qwe;
            if (w_asd > inner) inner = w_asd;
            if (w_fun > inner) inner = w_fun;
            if (w_spc > inner) inner = w_spc;

            const float kb_w = inner + 2.0f * pd;
            const float kb_h = 5.0f * key + 4.0f * gap + 2.0f * pd;
            const float sw = g_screen_w > 0 ? static_cast<float>(g_screen_w) : kb_w;
            const float sh = g_screen_h > 0 ? static_cast<float>(g_screen_h) : kb_h;
            float ox = (sw - kb_w) * 0.5f;
            if (ox < 0.0f) ox = 0.0f;
            float oy = sh - kb_h - 10.0f * g_scale;
            if (oy < 0.0f) oy = 0.0f;

            // 面板 + 边框
            draw_rect(ox - 2.0f * g_scale, oy - 2.0f * g_scale, kb_w + 4.0f * g_scale,
                      kb_h + 4.0f * g_scale, kBorder);
            draw_rect(ox, oy, kb_w, kb_h, kPanel);

            const float lx = ox + pd;
            bool consumed = false;
            float y = oy + pd;
            const float step = key + gap;

            // 数字行
            for (int i = 0; i < 10; ++i) {
                char label[2] = {kRow1[i], '\0'};
                draw_key(lx + i * step, y, key, key, label, kRow1[i], consumed);
            }
            y += step;
            // qwerty 行
            for (int i = 0; i < 10; ++i) {
                const char out = g_edit.shift ? static_cast<char>(std::toupper(kRow2[i])) : kRow2[i];
                char label[2] = {out, '\0'};
                draw_key(lx + i * step, y, key, key, label, out, consumed);
            }
            y += step;
            // asdf 行（水平居中）
            for (int i = 0; i < 9; ++i) {
                const char out = g_edit.shift ? static_cast<char>(std::toupper(kRow3[i])) : kRow3[i];
                char label[2] = {out, '\0'};
                draw_key(lx + (w_qwe - w_asd) * 0.5f + i * step, y, key, key, label, out, consumed);
            }
            y += step;
            // 功能行：shift + zxcvbnm + del（整行居中）
            {
                float x = lx + (inner - w_fun) * 0.5f;
                {
                    const bool hv = mouse_in(x, y, wide, key);
                    if (hv) g_hover_gui = true;
                    if (hv && g_mouse_released) g_edit.shift = !g_edit.shift;
                    draw_key_box(x, y, wide, key, g_edit.shift ? "ABC" : "abc", hv);
                    x += wide + gap;
                }
                for (int i = 0; i < 7; ++i) {
                    const char out = g_edit.shift ? static_cast<char>(std::toupper(kRow4[i])) : kRow4[i];
                    char label[2] = {out, '\0'};
                    draw_key(x, y, key, key, label, out, consumed);
                    x += step;
                }
                {
                    const bool hv = mouse_in(x, y, wide, key);
                    if (hv) g_hover_gui = true;
                    if (hv && g_mouse_released && !g_edit.buf.empty()) g_edit.buf.pop_back();
                    draw_key_box(x, y, wide, key, "del", hv);
                }
            }
            y += step;
            // 空格 / 完成
            {
                float x = lx + (inner - w_spc) * 0.5f;
                const float space_w = key * 6.0f;
                {
                    const bool hv = mouse_in(x, y, space_w, key);
                    if (hv) g_hover_gui = true;
                    if (hv && g_mouse_released) vkb_append(' ');
                    draw_key_box(x, y, space_w, key, "space", hv);
                    x += space_w + gap;
                }
                {
                    const float done_w = key * 3.0f;
                    const bool hv = mouse_in(x, y, done_w, key);
                    if (hv) g_hover_gui = true;
                    if (hv && g_mouse_released) g_edit.active = false;
                    draw_key_box(x, y, done_w, key, "done", hv);
                }
            }
            (void) consumed;
        }

        // ====================================================================
        // Lua GUI 函数
        // ====================================================================

        int l_gui_text(lua_State *L) {
            const char *s = luaL_checkstring(L, 1);
            if (!g_ctx) return 0;
            float x, y;
            const float w = text_width(s);
            const float h = line_height();
            place(x, y, w, h);
            draw_string(x, y, s, kText, 1.0f);
            return 0;
        }

        int l_gui_text_wrapped(lua_State *L) {
            const char *s = luaL_checkstring(L, 1);
            if (!g_ctx) return 0;
            // 按内容宽度粗略换行
            const float max_w = g_ctx->cw;
            float x, y;
            place(x, y, g_ctx->cw, line_height());
            float cur_x = x, cur_y = y;
            float line_w = 0.0f;
            std::string line;
            for (const char *p = s; *p;) {
                const size_t n = utf8_next(p);
                const std::string ch(p, n);
                const float cw = text_width(ch.c_str());
                if (line_w + cw > max_w && !line.empty()) {
                    draw_string(cur_x, cur_y, line.c_str(), kText, 1.0f);
                    cur_y += line_height();
                    cur_x = x;
                    line_w = 0.0f;
                    line.clear();
                }
                line += ch;
                line_w += cw;
                p += n;
            }
            if (!line.empty()) draw_string(cur_x, cur_y, line.c_str(), kText, 1.0f);
            g_ctx->prev_h = (cur_y - y) + line_height();
            return 0;
        }

        int l_gui_separator(lua_State *) {
            if (!g_ctx) return 0;
            advance_line();
            float x, y;
            place(x, y, g_ctx->cw, 2.0f);
            Color c;
            make_color(c, 120, 130, 170, 255);
            draw_rect(x, y, g_ctx->cw, 1.0f * g_scale, c);
            g_ctx->prev_h = 6.0f * g_scale;
            return 0;
        }

        int l_gui_spacing(lua_State *) {
            if (!g_ctx) return 0;
            advance_line();
            g_ctx->prev_h = line_height() * 0.5f;
            return 0;
        }

        int l_gui_same_line(lua_State *) {
            if (g_ctx) g_ctx->same_line = true;
            return 0;
        }

        int l_gui_button(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            if (!g_ctx) {
                lua_pushboolean(L, 0);
                return 1;
            }
            float x, y;
            const float w = text_width(label) + 24.0f * g_scale;
            const float h = button_height();
            place(x, y, w, h);

            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            bool clicked = false;
            if (hovered && g_mouse_released) {
                clicked = true;
            }
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);                                   // 按钮边框
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1,
                      hovered ? (g_mouse_down ? kBtnActive : kBtnHover) : kBtn);
            draw_string(x + 12.0f * g_scale, y + (h - line_height()) * 0.5f, label, kWhite, 1.0f);
            lua_pushboolean(L, clicked);
            return 1;
        }

        int l_gui_checkbox(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            bool value = lua_toboolean(L, 2) != 0;
            if (!g_ctx) {
                lua_pushboolean(L, value);
                return 1;
            }
            const float box = 18.0f * g_scale;
            const float h = line_height();
            const float w = box + item_gap() + text_width(label);
            float x, y;
            place(x, y, w, h);

            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            if (hovered && g_mouse_released) value = !value;

            const float by = y + (h - box) * 0.5f;
            const float g1 = 1.0f * g_scale;
            draw_rect(x, by, box, box, kTrackEdge);                                  // 外边框
            draw_rect(x + g1, by + g1, box - 2 * g1, box - 2 * g1, value ? kFill : kTrack);
            draw_string(x + box + item_gap(), y, label, kText, 1.0f);
            lua_pushboolean(L, value);
            return 1;
        }

        int l_gui_slider(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            float value = static_cast<float>(luaL_checknumber(L, 2));
            const float mn = static_cast<float>(luaL_optnumber(L, 3, 0.0));
            const float mx = static_cast<float>(luaL_optnumber(L, 4, 1.0));
            if (!g_ctx) {
                lua_pushnumber(L, value);
                return 1;
            }

            const float h = button_height();
            const float w = g_ctx->cw;
            float x, y;
            place(x, y, w, h);

            char text[128];
            std::snprintf(text, sizeof(text), "%s: %.2f", label, value);
            draw_string(x, y + (h - line_height()) * 0.5f, text, kText, 1.0f);

            // 轨道在右侧
            const float track_w = w * 0.45f;
            const float track_x = x + w - track_w;
            const float track_h = 10.0f * g_scale;
            const float track_y = y + (h - track_h) * 0.5f;

            const std::string id = g_current_mod + "#" + label;
            const bool hovered = mouse_in(track_x, y, track_w, h);
            if (hovered || g_slider_active == id) g_hover_gui = true;
            if (hovered && g_mouse_pressed) g_slider_active = id;
            if (!g_mouse_down && g_slider_active == id) g_slider_active.clear();

            float ratio = 0.0f;
            if (g_slider_active == id && g_mouse_down) {
                ratio = (g_mouse_x - track_x) / track_w;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                value = mn + (mx - mn) * ratio;
            } else {
                ratio = (mx != mn) ? (value - mn) / (mx - mn) : 0.0f;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
            }

            const float g1 = 1.0f * g_scale;
            draw_rect(track_x, track_y, track_w, track_h, kTrackEdge);           // 轨道边框
            draw_rect(track_x + g1, track_y + g1, track_w - 2 * g1, track_h - 2 * g1, kTrack);
            draw_rect(track_x + g1, track_y + g1, (track_w - 2 * g1) * ratio, track_h - 2 * g1, kFill);
            // 旋钮
            const float knob_w = 8.0f * g_scale;
            const float kx = track_x + g1 + (track_w - 2 * g1) * ratio - knob_w * 0.5f;
            draw_rect(kx, track_y - 2.0f * g_scale, knob_w, track_h + 4.0f * g_scale, kWhite);
            lua_pushnumber(L, value);
            return 1;
        }

        int l_gui_progress_bar(lua_State *L) {
            const float value = static_cast<float>(luaL_checknumber(L, 1));
            if (!g_ctx) return 0;
            float x, y;
            const float w = g_ctx->cw;
            const float h = 18.0f * g_scale;
            place(x, y, w, h);
            float ratio = value;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);                                   // 外边框
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1, kTrack);
            draw_rect(x + g1, y + g1, (w - 2 * g1) * ratio, h - 2 * g1, kFill);
            char ptxt[16];
            std::snprintf(ptxt, sizeof(ptxt), "%d%%", static_cast<int>(ratio * 100.0f + 0.5f));
            draw_string(x + (w - text_width(ptxt)) * 0.5f, y + (h - line_height()) * 0.5f, ptxt, kWhite, 1.0f);
            return 0;
        }

        int l_gui_collapsing_header(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            if (!g_ctx) {
                lua_pushboolean(L, 0);
                return 1;
            }
            const std::string id = g_current_mod + "#" + label;
            bool open = g_headers[id];
            float x, y;
            const float w = g_ctx->cw;
            const float h = button_height();
            place(x, y, w, h);
            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            if (hovered && g_mouse_released) open = !open;
            g_headers[id] = open;
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1, hovered ? kBtnHover : kBtn);
            const std::string text = (open ? "v " : "> ") + std::string(label);
            draw_string(x + 8.0f * g_scale, y + (h - line_height()) * 0.5f, text.c_str(), kWhite, 1.0f);
            lua_pushboolean(L, open);
            return 1;
        }

        int l_gui_input_text(lua_State *L) {
            const char *label = luaL_checkstring(L, 1);
            const char *value = luaL_optstring(L, 2, "");
            if (!g_ctx) {
                lua_pushstring(L, value);
                return 1;
            }
            const std::string id = g_current_mod + "#" + label;
            const bool editing = g_edit.active && g_edit.owner == id;

            float x, y;
            const float w = g_ctx->cw;
            const float h = button_height();
            place(x, y, w, h);
            const bool hovered = mouse_in(x, y, w, h);
            if (hovered) g_hover_gui = true;
            if (hovered && g_mouse_pressed) {
                g_edit.active = true;
                g_edit.owner = id;
                g_edit.buf = value;
            }

            const std::string shown = editing ? (g_edit.buf + "|") : std::string(value[0] ? value : "(tap to edit)");
            draw_rect(x, y, w, h, editing ? kBtnActive : (hovered ? kBtnHover : kBtn));
            draw_string(x + 8.0f * g_scale, y + (h - line_height()) * 0.5f, shown.c_str(), kWhite, 1.0f);

            lua_pushstring(L, editing ? g_edit.buf.c_str() : value);
            return 1;
        }

        // --------------------------------------------------------------------
        // 窗口定位 / 显示控制：允许 Mod 指定绘制位置、控制出现与消失
        // --------------------------------------------------------------------
        WindowState *current_window() {
            if (g_current_mod.empty()) return nullptr;
            return &g_windows[g_current_mod];
        }

        /// mod.gui.set_pos(x, y)  —— 设置窗口左上角（屏幕像素）
        int l_gui_set_pos(lua_State *L) {
            const float x = static_cast<float>(luaL_checknumber(L, 1));
            const float y = static_cast<float>(luaL_checknumber(L, 2));
            if (WindowState *ws = current_window()) {
                ws->x = x;
                ws->y = y;
            }
            return 0;
        }

        /// mod.gui.get_pos() -> x, y
        int l_gui_get_pos(lua_State *L) {
            WindowState *ws = current_window();
            lua_pushnumber(L, ws ? ws->x : 0.0);
            lua_pushnumber(L, ws ? ws->y : 0.0);
            return 2;
        }

        /// mod.gui.set_width(w) —— 设置窗口宽度（高度由内容自动撑开）
        int l_gui_set_width(lua_State *L) {
            const float w = static_cast<float>(luaL_checknumber(L, 1));
            if (WindowState *ws = current_window()) {
                if (w > 60.0f) ws->w = w;
            }
            return 0;
        }

        /// mod.gui.set_cursor(x, y) —— 移动当前布局光标（相对窗口内容区左上角），
        /// 之后绘制的控件从该位置开始（用于自由摆放）
        int l_gui_set_cursor(lua_State *L) {
            const float x = static_cast<float>(luaL_checknumber(L, 1));
            const float y = static_cast<float>(luaL_checknumber(L, 2));
            if (g_ctx) {
                g_ctx->cx = x;
                g_ctx->cy = y;
                g_ctx->prev_h = 0.0f;
                g_ctx->same_line = false;
            }
            return 0;
        }

        /// mod.gui.set_visible(v) -> v
        int l_gui_set_visible(lua_State *L) {
            const bool v = lua_toboolean(L, 1) != 0;
            if (WindowState *ws = current_window()) ws->visible = v;
            lua_pushboolean(L, v);
            return 1;
        }

        /// mod.gui.is_visible() -> bool
        int l_gui_is_visible(lua_State *L) {
            WindowState *ws = current_window();
            lua_pushboolean(L, ws && ws->visible);
            return 1;
        }

        int l_gui_show(lua_State *) {
            if (WindowState *ws = current_window()) ws->visible = true;
            return 0;
        }

        int l_gui_hide(lua_State *) {
            if (WindowState *ws = current_window()) ws->visible = false;
            return 0;
        }

        const luaL_Reg gui_funcs[] = {
                {"text", l_gui_text},
                {"text_wrapped", l_gui_text_wrapped},
                {"button", l_gui_button},
                {"checkbox", l_gui_checkbox},
                {"slider", l_gui_slider},
                {"same_line", l_gui_same_line},
                {"separator", l_gui_separator},
                {"spacing", l_gui_spacing},
                {"collapsing_header", l_gui_collapsing_header},
                {"progress_bar", l_gui_progress_bar},
                {"input_text", l_gui_input_text},
                {"set_pos", l_gui_set_pos},
                {"get_pos", l_gui_get_pos},
                {"set_width", l_gui_set_width},
                {"set_cursor", l_gui_set_cursor},
                {"set_visible", l_gui_set_visible},
                {"is_visible", l_gui_is_visible},
                {"show", l_gui_show},
                {"hide", l_gui_hide},
                {nullptr, nullptr},
        };

        // ====================================================================
        // 帧钩子
        // ====================================================================

        patch_hook_id_t g_draw_hook = PATCH_HOOK_INVALID_ID;
        patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
        bool g_installed = false;
        bool g_in_render = false;

        /// 当前指针是否落在任一已打开窗口内（用上一帧缓存的位置）
        bool pointer_over_windows() {
            if (g_windows.empty() || !g_gfx.usable) return false;
            if (!g_gfx.a_mouse_x.valid() || !g_gfx.a_mouse_left.valid()) return false;
            const float mx = static_cast<float>(read_int(g_gfx.a_mouse_x, 0));
            const float my = static_cast<float>(read_int(g_gfx.a_mouse_y, 0));
            for (const auto &kv: g_windows) {
                const WindowState &ws = kv.second;
                if (!ws.visible) continue;
                if (mx >= ws.x && mx <= ws.x + ws.w && my >= ws.y && my <= ws.y + ws.h) return true;
            }
            return false;
        }

        /// Main.Update 前缀：指针悬停在 GUI 上时屏蔽游戏鼠标，避免误触游戏操作
        bool update_prefix(patch_handle_t, void **, const patch_method_signature_t *, void *) {
            if (pointer_over_windows()) {
                write_bool(g_gfx.a_mouse_left, false);
                write_bool(g_gfx.a_mouse_right, false);
            }
            return false; // 不跳过原函数
        }

        void render() {
            resolve_gfx();
            if (!g_gfx.usable) return;

            // 没有任何带 on_gui 的 Mod 时完全不介入渲染
            bool any_gui = false;
            for (auto &kv: core::lua_mod_handles) {
                if (kv.second && kv.second->gui_ref != LUA_NOREF) {
                    any_gui = true;
                    break;
                }
            }
            if (!any_gui) return;

            // 启动画面（showSplash）阶段贴图/字体尚未就绪，此时绘制会导致
            // DrawInvBG / DrawBorderString 解引用空资源而崩溃，必须跳过。
            if (read_bool(g_gfx.a_show_splash, false)) return;

            update_screen_size();
            update_input();
            g_hover_gui = false;

            patch_handle_t sb = read_static_object(g_gfx.f_sprite_batch);
            if (!sb || !patchlib_is_valid(sb)) return;

            g_sb = sb;
            sb_begin();

            for (auto &kv: core::lua_mod_handles) {
                draw_mod_gui(kv.second);
                // 拖动释放
                if (!g_mouse_down) g_drag_id.clear();
            }

            if (g_edit.active) draw_virtual_keyboard();

            sb_end();

            g_sb = nullptr;

            // 指针悬停在 GUI 上时，屏蔽游戏的鼠标左键，避免同时触发游戏操作
            if (g_hover_gui) {
                write_bool(g_gfx.a_mouse_left, false);
                write_bool(g_gfx.a_mouse_right, false);
            }
        }

        void frame_postfix(patch_handle_t, void **, void *, const patch_method_signature_t *) {
            if (g_in_render) return;
            g_in_render = true;
            render();
            g_in_render = false;
        }

    } // namespace

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
        if (!render_target || !patchlib_is_valid(render_target)) render_target = patchlib_type_get_method(main_type, "Draw");
        if (!render_target || !patchlib_is_valid(render_target)) {
            LOG_WARN("[gui] render hook target not found, native GUI disabled");
            return;
        }
        g_draw_hook = patchlib_install_prepost_hook(render_target, nullptr, frame_postfix);
        if (g_draw_hook == PATCH_HOOK_INVALID_ID) {
            LOG_ERROR("[gui] failed to install render hook");
            return;
        }

        // 输入屏蔽：在 Main.Update 之前判断是否悬停 GUI
        patch_handle_t update = patchlib_type_get_method_by_param_count(main_type, "Update", 1);
        if (!update || !patchlib_is_valid(update)) update = patchlib_type_get_method_by_param_count(main_type, "DoUpdate", 1);
        if (update && patchlib_is_valid(update)) {
            g_update_hook = patchlib_install_prepost_hook(update, update_prefix, nullptr);
            LOG_INFO("[gui] hooked Main.Update for input suppression (id={})", static_cast<int>(g_update_hook));
        } else {
            LOG_WARN("[gui] Main.Update not found; GUI will not suppress game mouse input");
        }

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
        g_installed = false;
    }

    void register_api(lua_State *L, lua_mod_handle_t *) {
        // 在 mod 表（栈顶）上挂 mod.gui
        lua_newtable(L);
        luaL_setfuncs(L, gui_funcs, 0);
        lua_setfield(L, -2, "gui");
    }

} // namespace lualoader::gui
