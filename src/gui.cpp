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
#include "controls.hpp"
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

        // System.Nullable<Matrix>（用于强制 SpriteBatch.Begin 使用单位矩阵）
        // .NET 布局：{ bool hasValue; T value; }（value 按 T 对齐）
        struct Mat4 {
            float m[16];
        };
        struct NullableMat {
            unsigned char has_value;
            unsigned char _pad[3];
            Mat4 value;
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
            patch_handle_t m_sb_begin8{nullptr}; // Begin(SpriteSortMode,BlendState,...,Nullable<Matrix>,bool)
            patch_handle_t m_sb_end{nullptr};
            int sb_begin_args{0};

            patch_handle_t m_draw_inv_bg{nullptr};     // Utils.DrawInvBG(SpriteBatch,Rectangle,Color)
            patch_handle_t m_draw_border_string{nullptr}; // Utils.DrawBorderString(...)

            Accessor a_screen_w, a_screen_h;
            Accessor a_mouse_x, a_mouse_y;
            Accessor a_mouse_left, a_mouse_left_release, a_mouse_right,a_scroll_wheel;
            Accessor a_show_splash;
#if defined(__ANDROID__)
            patch_handle_t m_setup_rendering_ui{nullptr};
            patch_handle_t m_adjust_real_screen{nullptr};
            patch_handle_t f_cm_instance{nullptr};
            patch_handle_t m_cm_get_num_cursors{nullptr};
            patch_handle_t m_cm_get_cursor{nullptr};
            patch_handle_t f_cursor_pos{nullptr};
            patch_handle_t f_cursor_down{nullptr};
            patch_handle_t f_cursor_id{nullptr};
            patch_handle_t f_cursor_ignore_world{nullptr};
            patch_handle_t f_cursor_ignore{nullptr};
#endif
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
                if(auto input=patchlib_type_get_type("Terraria.GameInput","PlayerInput"))g_gfx.a_scroll_wheel=resolve_accessor(input,"ScrollWheelDelta");
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
                g_gfx.m_sb_begin8 = get_method(t_sb, "Begin", 8);
            }

            const patch_handle_t t_utils = patchlib_type_get_type("Terraria", "Utils");
            if (t_utils) {
                g_gfx.m_draw_inv_bg = get_method(t_utils, "DrawInvBG", 3);
                g_gfx.m_draw_border_string = get_method(t_utils, "DrawBorderString", 8);
            }

#if defined(__ANDROID__)
            const patch_handle_t t_runner = patchlib_type_get_type("", "XNAUnityRunner");
            if (t_runner) {
                g_gfx.m_setup_rendering_ui = get_method(t_runner, "SetupRenderingUI", 1);
                g_gfx.m_adjust_real_screen = get_method(t_runner, "AdjustRealScreenForUI", 1);
            }

            const patch_handle_t t_cm = patchlib_type_get_type("", "CursorManager");
            if (t_cm) {
                g_gfx.f_cm_instance = patchlib_type_get_field(t_cm, "Instance");
                g_gfx.m_cm_get_num_cursors = get_method(t_cm, "GetNumCursors", 0);
                g_gfx.m_cm_get_cursor = get_method(t_cm, "GetCursor", 1);
            }

            const patch_handle_t t_cursor = patchlib_type_get_type("", "Cursor");
            if (t_cursor) {
                g_gfx.f_cursor_pos = patchlib_type_get_field(t_cursor, "Position");
                g_gfx.f_cursor_down = patchlib_type_get_field(t_cursor, "Down");
                g_gfx.f_cursor_id = patchlib_type_get_field(t_cursor, "Id");
                g_gfx.f_cursor_ignore_world = patchlib_type_get_field(t_cursor, "IgnoreForWorld");
                g_gfx.f_cursor_ignore = patchlib_type_get_field(t_cursor, "Ignore");
            }
#endif

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
            enum Kind { RECT, TEXT, IMAGE } kind;
            float x, y, w, h; // RECT 用 w/h；TEXT 用 scale
            Color col;
            std::string text;
            float scale;
            int sx=0,sy=0,sw=0,sh=0,border=0;
            float edge=0;
            Viewport clip;
        };

        Viewport g_viewport;
        bool g_scroll_drag=false;
        bool text_in_clip(float x,float y,const char *text,float scale);
        std::vector<DrawCmd> g_cmds;
        bool g_recording = false;
        std::vector<std::string> g_image_release_queue;

#if defined(__ANDROID__)
        // Android 兜底：内核 patchlib 的按值结构体 FFI 编组在含静态字段的结构体
        // （Vector2/Color/Rectangle 都有）上会生成错误的 ffi_type，导致托管方法
        // 收到错位参数并抛异常（→ std::terminate → SIGABRT）。
        // 这里直接用原生函数指针按正确的 C ABI 调用，绕开 patch_type_to_ffi_type。
        // 详见 TEFKernel/documents/patchlib-ffi-struct-bug.md。
        using FnDrawInvBG = void (*)(void *sb, Recti r, Color c);
        using FnDrawString = Vec2 (*)(void *sb, void *text, Vec2 pos, Color color,
                                      float scale, float ax, float ay, int32_t max_chars);
        using FnAdjustRealScreenForUI = Vec2 (*)(Vec2 pos);
        using FnGetNumCursors = int (*)(void *self);
        using FnGetCursor = void * (*)(void *self, int index);
#endif

        void exec_rect(const float x, const float y, const float w, const float h, const Color &c) {
            if (!g_gfx.m_draw_inv_bg || !g_sb || w <= 0 || h <= 0) return;
            if(g_viewport.enabled){float cx=x,cy=y,cw=w,ch=h;if(g_viewport.crop(cx,cy,cw,ch))images::solid(g_sb,cx,cy,cw,ch,&c);return;}
            if(w<24.0f || h<24.0f){images::solid(g_sb,x,y,w,h,&c);return;}
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

        struct CachedText {patch_handle_t value=nullptr;unsigned int pin=0;unsigned long long used=0;};
        std::unordered_map<std::string,CachedText> g_text_cache;
        unsigned long long g_text_clock=0;
        void release_text(CachedText &entry){if(entry.pin)images::release_object(entry.pin);if(entry.value)patchlib_free(entry.value);entry={};}
        void clear_text_cache(){for(auto &kv:g_text_cache)release_text(kv.second);g_text_cache.clear();g_text_clock=0;}
        patch_handle_t cached_string(const char *text,bool &temporary){
            temporary=false;auto it=g_text_cache.find(text);
            if(it!=g_text_cache.end()){it->second.used=++g_text_clock;return it->second.value;}
            auto value=patchlib_string_create(text);if(!value)return nullptr;
            // Long dynamic content is not retained; fixed labels are bounded to 256 entries.
            if(std::strlen(text)>512){temporary=true;return value;}
            CachedText entry;entry.value=value;entry.used=++g_text_clock;
#if defined(__ANDROID__)
            entry.pin=images::retain_object(value);
            if(!entry.pin){temporary=true;return value;}
#endif
            if(g_text_cache.size()>=256){auto old=g_text_cache.begin();for(auto i=g_text_cache.begin();i!=g_text_cache.end();++i)if(i->second.used<old->second.used)old=i;release_text(old->second);g_text_cache.erase(old);}
            g_text_cache.emplace(text,entry);return value;
        }
        void exec_string(const float x, const float y, const char *text, const Color &c, const float scale) {
            if (!g_gfx.m_draw_border_string || !g_sb || !text || !*text) return;
            bool temporary=false;patch_handle_t str=cached_string(text,temporary);
            if (!str) return;
            struct TempStringGuard{patch_handle_t value;bool temporary;~TempStringGuard(){if(temporary)patchlib_free(value);}} string_guard{str,temporary};
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

        }

        void sb_begin();
        void sb_end();

        /// Window backgrounds are one batch; text is a separate foreground batch.
        /// PE's Unity compatibility renderer may regroup textures in deferred mode.
        void flush_cmds() {
            bool has_text=false;
            const auto saved_clip=g_viewport;
            for (const DrawCmd &cmd: g_cmds) {
                g_viewport=cmd.clip;images::set_clip(cmd.clip.enabled,cmd.clip.x,cmd.clip.y,cmd.clip.w,cmd.clip.h);
                if (cmd.kind == DrawCmd::RECT) {
                    exec_rect(cmd.x, cmd.y, cmd.w, cmd.h, cmd.col);
                } else if (cmd.kind == DrawCmd::IMAGE) {
                    images::draw(g_sb,cmd.text,cmd.x,cmd.y,cmd.w,cmd.h,&cmd.col,
                                 cmd.sx,cmd.sy,cmd.sw,cmd.sh,cmd.border,cmd.edge);
                } else has_text=true;
            }
            if(has_text){
                sb_end();
                sb_begin();
                for(const DrawCmd &cmd:g_cmds)if(cmd.kind==DrawCmd::TEXT){
                    g_viewport=cmd.clip;
                    if(text_in_clip(cmd.x,cmd.y,cmd.text.c_str(),cmd.scale))exec_string(cmd.x,cmd.y,cmd.text.c_str(),cmd.col,cmd.scale);
                }
            }
            g_cmds.clear();g_viewport=saved_clip;images::set_clip(false,0,0,0,0);
        }

        Color themed_color(const Color &c);

        void draw_rect(const float x, const float y, const float w, const float h, const Color &c) {
            if (g_recording) {
                if(!g_viewport.intersects(x,y,w,h))return;
                g_cmds.push_back(DrawCmd{DrawCmd::RECT, x, y, w, h, themed_color(c), {}, 0.0f});g_cmds.back().clip=g_viewport;
                return;
            }
            exec_rect(x, y, w, h, themed_color(c));
        }

        void draw_string(const float x, const float y, const char *text, const Color &c,
                         const float scale = 1.0f) {
            if (!text || !*text) return;
            if (g_recording) {
                if(!text_in_clip(x,y,text,scale))return;
                g_cmds.push_back(DrawCmd{DrawCmd::TEXT, x, y, 0.0f, 0.0f, themed_color(c), text, scale});g_cmds.back().clip=g_viewport;
                return;
            }
            exec_string(x, y, text, themed_color(c), scale);
        }

        void draw_image(const std::string &id,float x,float y,float w,float h,const Color &color,
                        int sx=0,int sy=0,int sw=0,int sh=0,int border=0,float edge=0) {
            if(id.empty() || w<=0 || h<=0||!g_viewport.intersects(x,y,w,h))return;
            if(g_recording){DrawCmd cmd{DrawCmd::IMAGE,x,y,w,h,color,id,0};
                cmd.sx=sx;cmd.sy=sy;cmd.sw=sw;cmd.sh=sh;cmd.border=border;cmd.edge=edge;cmd.clip=g_viewport;g_cmds.push_back(cmd);
            }else images::draw(g_sb,id,x,y,w,h,&color,sx,sy,sw,sh,border,edge);
        }
        void sb_begin() {
            if (!g_sb) return;
#if defined(__ANDROID__)
            // PE：强制使用单位矩阵，避免 GUI 跟随世界相机/视野缩放
            // （PE 无 0 参 Begin；2 参 Begin 会带上世界变换）
            if (g_gfx.m_sb_begin8) {
                static NullableMat ident = [] {
                    NullableMat n{};
                    n.has_value = 1;
                    n.value.m[0] = n.value.m[5] = n.value.m[10] = n.value.m[15] = 1.0f;
                    return n;
                }();
                using FnBegin8 = void (*)(void *self, int sort_mode, void *blend, void *sampler,
                                          void *depth, void *raster, void *effect, NullableMat *xform,
                                          bool deferred);
                auto fn = reinterpret_cast<FnBegin8>(patchlib_method_get_pointer(g_gfx.m_sb_begin8));
                if (fn) {
                    bool deferred = true;
                    fn(g_sb, 0, nullptr, nullptr, nullptr, nullptr, nullptr, &ident, deferred);
                    return;
                }
            }
#endif
            if (!g_gfx.m_sb_begin) return;
            if (g_gfx.sb_begin_args == 0) {
                patchlib_method_invoke_args(g_gfx.m_sb_begin, g_sb, nullptr, nullptr);
            } else if (g_gfx.sb_begin_args == 2) {
                int sort_mode = 0; // SpriteSortMode.Deferred
                unsigned char deferred = 1;
                void *args[2] = {&sort_mode, &deferred};
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
        std::string g_render_owner,g_input_owner,g_pointer_owner;
        ClickState g_click_state;
        int g_next_window_order=0;
#if defined(__ANDROID__)
        int g_active_cursor_id=0;
        bool g_active_cursor_valid=false;
#endif

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

        bool point_in_gui(float x, float y);

        // 前向声明：这两个状态在后面（窗口状态区）才定义，update_input 会用到
        extern std::string g_drag_id;
        extern std::string g_slider_active;

#if defined(__ANDROID__)
        bool cursor_captured(void *cursor);
        void release_finished_touches();
#endif
        void update_input() {
#if defined(__ANDROID__)
            bool android_touch_handled = false;
            void *cm_instance = g_gfx.f_cm_instance ? read_static_object(g_gfx.f_cm_instance) : nullptr;
            if (cm_instance && g_gfx.m_cm_get_num_cursors && g_gfx.m_cm_get_cursor &&
                g_gfx.m_adjust_real_screen && g_gfx.f_cursor_pos && g_gfx.f_cursor_down) {
                auto fn_num_cursors = reinterpret_cast<FnGetNumCursors>(patchlib_method_get_pointer(g_gfx.m_cm_get_num_cursors));
                auto fn_get_cursor = reinterpret_cast<FnGetCursor>(patchlib_method_get_pointer(g_gfx.m_cm_get_cursor));
                auto fn_adjust = reinterpret_cast<FnAdjustRealScreenForUI>(patchlib_method_get_pointer(g_gfx.m_adjust_real_screen));

                if (fn_num_cursors && fn_get_cursor && fn_adjust) {
                    const int num = fn_num_cursors(cm_instance);
                    void *active_cursor = nullptr;
                    Vec2 active_ui_pos{g_mouse_x, g_mouse_y};
                    bool touch_down = false;

                    for (int i = 0; i < num; ++i) {
                        void *c = fn_get_cursor(cm_instance, i);
                        if (!c) continue;
                        auto *pos_ptr = static_cast<Vec2 *>(patchlib_field_get_pointer(g_gfx.f_cursor_pos, c));
                        auto *down_ptr = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_down, c));
                        if (!pos_ptr || !down_ptr) continue;

                        auto *id_ptr=g_gfx.f_cursor_id?static_cast<int*>(patchlib_field_get_pointer(g_gfx.f_cursor_id,c)):nullptr;
                        if(g_active_cursor_valid && (!id_ptr || *id_ptr!=g_active_cursor_id))continue;
                        const bool is_down = (*down_ptr != 0);
                        const Vec2 ui_pos = fn_adjust(*pos_ptr);
                        const bool in_gui = point_in_gui(ui_pos.x, ui_pos.y);

                        if (is_down) {
                            if (in_gui || cursor_captured(c) || (!g_drag_id.empty() && g_prev_mouse_down) || (!g_slider_active.empty() && g_prev_mouse_down)) {
                                active_cursor = c;
                                if(id_ptr){g_active_cursor_id=*id_ptr;g_active_cursor_valid=true;}
                                active_ui_pos = ui_pos;
                                touch_down = true;

                                if (g_gfx.f_cursor_ignore_world) {
                                    auto *p = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_ignore_world, c));
                                    if (p) *p = 1;
                                }
                                if (g_gfx.f_cursor_ignore) {
                                    auto *p = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_ignore, c));
                                    if (p) *p = 1;
                                }
                                break;
                            }
                        } else if (g_prev_mouse_down && (in_gui || cursor_captured(c) || !g_drag_id.empty() || !g_slider_active.empty())) {
                            active_cursor = c;
                            active_ui_pos = ui_pos;
                            touch_down = false;
                            break;
                        }
                    }

                    if (active_cursor || g_prev_mouse_down) {
                        android_touch_handled = true;
                        if (active_cursor) {
                            g_mouse_x = active_ui_pos.x;
                            g_mouse_y = active_ui_pos.y;
                        }
                        g_mouse_down = touch_down;
                        g_mouse_pressed = g_mouse_down && !g_prev_mouse_down;
                        g_mouse_released = active_cursor && !g_mouse_down && g_prev_mouse_down;
                        g_prev_mouse_down = g_mouse_down;

                        if (!g_mouse_down && !g_mouse_released) {
                            g_mouse_x = -FLT_MAX;
                            g_mouse_y = -FLT_MAX;
                        }
                    }
                }
            }

            if (!android_touch_handled) {
                const bool raw_down = read_bool(g_gfx.a_mouse_left, false);
                if (raw_down) {
                    g_mouse_x = static_cast<float>(read_int(g_gfx.a_mouse_x, 0));
                    g_mouse_y = static_cast<float>(read_int(g_gfx.a_mouse_y, 0));
                }
                g_mouse_down = raw_down;
                g_mouse_pressed = g_mouse_down && !g_prev_mouse_down;
                g_mouse_released = !g_mouse_down && g_prev_mouse_down;
                g_prev_mouse_down = g_mouse_down;
                if (!g_mouse_down && !g_mouse_released) {
                    g_mouse_x = -FLT_MAX;
                    g_mouse_y = -FLT_MAX;
                }
            }
#else
            g_mouse_x = static_cast<float>(read_int(g_gfx.a_mouse_x, 0));
            g_mouse_y = static_cast<float>(read_int(g_gfx.a_mouse_y, 0));
            g_mouse_down = read_bool(g_gfx.a_mouse_left, false);
            g_mouse_pressed = g_mouse_down && !g_prev_mouse_down;
            g_mouse_released = !g_mouse_down && g_prev_mouse_down;
            g_prev_mouse_down = g_mouse_down;
#endif
        }

        bool mouse_in(const float x, const float y, const float w, const float h) {
            if(g_scroll_drag||!g_viewport.contains(g_mouse_x,g_mouse_y))return false;
            if (!g_render_owner.empty() && g_render_owner!=g_input_owner)return false;
            if (g_mouse_x == -FLT_MAX) return false;
            return g_mouse_x >= x && g_mouse_x <= x + w && g_mouse_y >= y && g_mouse_y <= y + h;
        }

        bool widget_click(const char *label,float x,float y,bool hover){
            if(g_scroll_drag)return false;
            const std::string owner=g_render_owner.empty()?"keyboard":g_render_owner;
            const std::string id=owner+"#"+label+"@"+std::to_string(static_cast<int>(x))+","+std::to_string(static_cast<int>(y));
            return g_click_state.update(id,hover,g_mouse_pressed,g_mouse_released);
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

        // 删除末尾一个完整 UTF-8 字符（退格用；避免只删一字节留下乱码）
        void utf8_pop_back(std::string &s) {
            if (s.empty()) return;
            size_t i = s.size();
            size_t cont = 0;
            while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80 && cont < 3) {
                --i;
                ++cont;
            }
            if (i > 0) --i;
            s.erase(i);
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

        bool text_in_clip(float x,float y,const char *text,float scale){
            return g_viewport.covers(x,y,text_width(text,scale),28.0f*g_scale*scale);
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

        struct ScrollFrame {Ctx *parent=nullptr;Ctx child;Viewport old_clip;bool old_drag=false;ScrollMotion *motion=nullptr;Viewport bounds;float content_w=0,content_h=0;};
        std::unordered_map<std::string,ScrollMotion> g_scrolls;
        std::deque<ScrollFrame> g_scroll_stack;
        std::string g_active_scroll;
        int g_wheel_delta=0;bool g_wheel_used=false;
        // ====================================================================
        // 每个 Mod 的窗口状态
        // ====================================================================

        struct WindowState {
            float x{40}, y{40};
            float w{380}, h{160};
            bool visible{true};   ///< 是否显示（Mod 可控制出现/消失）
            std::unordered_map<std::string, Color> colors;
            std::string title;
            std::string skin_panel,skin_icon;
            int skin_border=16;
            float skin_edge=0;
            float icon_size=48;
            IconGesture icon_gesture;
            std::string compact_label; // opt-in: square collapsed launcher
            bool collapsed{false}; ///< 是否收起为标题栏
            int order{0};
            bool initial_position=true;
        };

        std::unordered_map<std::string, WindowState> g_windows;

        struct IconFolder {std::string id;float x=40,y=40;std::vector<std::string> members;IconGesture gesture;};
        std::vector<IconFolder> g_folders;
        std::string g_open_folder,g_merge_target,g_merge_source,g_layout_path;
        std::unordered_map<std::string,IconGesture> g_member_gestures;
        std::unordered_map<std::string,std::pair<float,float>> g_saved_positions;
        std::unordered_map<std::string,bool> g_position_applied;
        std::string g_member_drag;float g_member_press_time=0;
        auto g_merge_since=std::chrono::steady_clock::now();
        int g_folder_serial=0;bool g_folder_layout_dirty=false;
        bool window_live(const std::string &id){for(const auto &kv:core::lua_mod_handles)if(kv.second&&kv.second->mod_id==id&&kv.second->gui_ref!=LUA_NOREF)return true;return false;}
        std::vector<std::string> folder_members(const IconFolder &f){std::vector<std::string> v;for(auto &id:f.members){auto it=g_windows.find(id);if(window_live(id)&&it!=g_windows.end()&&it->second.visible)v.push_back(id);}return v;}
        bool grouped_icon(const std::string &id){
            auto it=g_windows.find(id);if(it==g_windows.end()||!it->second.collapsed)return false;
            for(const auto &f:g_folders)if(folder_members(f).size()>=2&&std::find(f.members.begin(),f.members.end(),id)!=f.members.end())return true;
            return false;
        }
        Viewport folder_icon(const IconFolder &f){float sz=48*g_scale;return {true,f.x,f.y,sz,sz};}
        Viewport folder_panel(const IconFolder &f){
            const float cell=64*g_scale,w=3*cell+16*g_scale,h=3*cell+40*g_scale;
            return {true,std::clamp(f.x,0.0f,std::max(0.0f,g_screen_w-w)),std::clamp(f.y+52*g_scale,0.0f,std::max(0.0f,g_screen_h-h)),w,h};
        }
        std::unordered_map<std::string,int> g_folder_pages;
        std::string folder_hit(float x,float y){for(auto it=g_folders.rbegin();it!=g_folders.rend();++it){if(folder_members(*it).size()<2)continue;
            if(folder_icon(*it).contains(x,y)||(it->id==g_open_folder&&folder_panel(*it).contains(x,y)))return "#folder:"+it->id;}return {};}
        auto g_layout_retry=std::chrono::steady_clock::time_point::min();
        void save_layout(){
            if(g_layout_path.empty()||!g_folder_layout_dirty||std::chrono::steady_clock::now()<g_layout_retry)return;
            try{nlohmann::json j;j["version"]=1;j["folders"]=nlohmann::json::array();
                for(const auto &f:g_folders)j["folders"].push_back({{"id",f.id},{"x",f.x},{"y",f.y},{"members",f.members}});
                for(const auto &kv:g_windows){g_saved_positions[kv.first]={kv.second.x,kv.second.y};}
                for(const auto &kv:g_saved_positions)j["positions"][kv.first]={kv.second.first,kv.second.second};
                const auto tmp=g_layout_path+".tmp";std::ofstream out(tmp,std::ios::trunc);out<<j.dump(2);out.close();
                if(!out)throw std::runtime_error("layout write failed");
                std::error_code ec;std::filesystem::rename(tmp,g_layout_path,ec);
                if(ec){
                    const auto backup=g_layout_path+".bak";ec.clear();
                    if(std::filesystem::exists(g_layout_path))std::filesystem::rename(g_layout_path,backup,ec);
                    if(ec)throw std::runtime_error("cannot preserve old layout");
                    std::filesystem::rename(tmp,g_layout_path,ec);
                    if(ec){std::error_code restore;std::filesystem::rename(backup,g_layout_path,restore);throw std::runtime_error("cannot replace layout");}
                    std::filesystem::remove(backup,ec);
                }
                g_folder_layout_dirty=false;
            }catch(const std::exception &e){LOG_WARN("[gui-folder] save failed: {}",e.what());g_layout_retry=std::chrono::steady_clock::now()+std::chrono::seconds(5);}
        }
        void load_layout(const std::string &root){
            g_layout_path=(std::filesystem::path(root)/"gui_layout.json").string();g_folders.clear();g_saved_positions.clear();g_position_applied.clear();g_folder_serial=0;
            try{std::ifstream in(g_layout_path);if(!in)return;auto j=nlohmann::json::parse(in);if(j.value("version",0)!=1)return;
                std::unordered_map<std::string,bool> seen;
                if(j.contains("folders"))for(const auto &v:j["folders"]){if(g_folders.size()>=64)break;IconFolder f;f.id=std::to_string(++g_folder_serial);f.x=v.value("x",40.0f);f.y=v.value("y",40.0f);if(!std::isfinite(f.x)||!std::isfinite(f.y))continue;
                    for(const auto &m:v.at("members")){auto id=m.get<std::string>();if(id.size()>256||seen[id]||f.members.size()>=64)continue;seen[id]=true;f.members.push_back(id);}
                    if(f.members.size()>=2)g_folders.push_back(std::move(f));}
                if(j.contains("positions"))for(auto it=j["positions"].begin();it!=j["positions"].end();++it){if(g_saved_positions.size()>=4096)break;
                    float x=it.value().at(0).get<float>(),y=it.value().at(1).get<float>();if(std::isfinite(x)&&std::isfinite(y))g_saved_positions[it.key()]={x,y};}
            }catch(const std::exception &e){LOG_WARN("[gui-folder] invalid layout: {}",e.what());g_folders.clear();}
        }
        void merge_icon(const std::string &source,const std::string &target){
            if(source.empty()||source==target)return;
            if(target.rfind("#folder:",0)==0){for(auto &f:g_folders)if("#folder:"+f.id==target&&f.members.size()<64){f.members.push_back(source);break;}}
            else {if(g_folders.size()>=64)return;IconFolder f;f.id=std::to_string(++g_folder_serial);f.x=g_windows[target].x;f.y=g_windows[target].y;f.members={target,source};g_folders.push_back(std::move(f));}
            g_merge_target.clear();g_merge_source.clear();g_folder_layout_dirty=true;
        }
        void track_merge(const std::string &source){
            auto &ws=g_windows[source];std::string target;
            const float size=ws.skin_icon.empty()?title_height():ws.icon_size;
            const float cx=ws.x+size/2,cy=ws.y+size/2;
            for(const auto &f:g_folders)if(folder_members(f).size()>=2&&folder_icon(f).contains(cx,cy)){target="#folder:"+f.id;break;}
            if(target.empty())for(const auto &kv:g_windows){if(kv.first==source||!kv.second.collapsed||!kv.second.visible||grouped_icon(kv.first))continue;
                const float size=kv.second.skin_icon.empty()?title_height():kv.second.icon_size;
                Viewport box{true,kv.second.x,kv.second.y,size,size};if(box.contains(cx,cy)){target=kv.first;break;}}
            if(source!=g_merge_source||target!=g_merge_target){g_merge_source=source;g_merge_target=target;g_merge_since=std::chrono::steady_clock::now();}
            if(g_mouse_released&&!target.empty()&&std::chrono::steady_clock::now()-g_merge_since>=std::chrono::milliseconds(350))merge_icon(source,target);
        }
        void draw_folders();
        WindowState *g_style_window = nullptr;
        Color themed_color(const Color &c) {
            if (!g_style_window) return c;
            const char *key = &c == &kPanel ? "panel" : &c == &kTitle ? "title" :
                &c == &kBtn ? "button" : &c == &kBtnHover ? "hover" :
                &c == &kBtnActive ? "active" : &c == &kBorder ? "border" :
                &c == &kTrackEdge ? "border" : &c == &kText ? "text" : nullptr;
            if (!key) return c;
            auto it = g_style_window->colors.find(key);
            return it == g_style_window->colors.end() ? c : it->second;
        }
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
            // 内置拼音输入法（PC：硬件键盘打拼音 -> 候选栏选词）
            std::string pinyin;              // 当前拼音串（小写字母）
            std::vector<std::string> cands;  // 当前候选（最多 90，按页展示）
            bool ime_on{true};               // 中/英（Shift 单击切换）
            size_t page{0};                  // 候选翻页
        };
        EditState g_edit;

        void draw_folders(){
            g_viewport={};g_scroll_drag=false;g_style_window=nullptr;g_recording=true;
            if(g_mouse_pressed&&folder_hit(g_mouse_x,g_mouse_y).empty()){
                g_open_folder.clear();g_member_gestures.clear();g_member_drag.clear();
            }
            for(auto &f:g_folders){auto members=folder_members(f);if(members.size()<2)continue;
                const float size=48*g_scale;f.x=std::clamp(f.x,0.0f,std::max(0.0f,g_screen_w-size));f.y=std::clamp(f.y,0.0f,std::max(0.0f,g_screen_h-size));
                const std::string owner="#folder:"+f.id;g_render_owner=owner;
                const bool hover=mouse_in(f.x,f.y,size,size);if(hover)g_hover_gui=true;
                auto move=f.gesture.update(g_mouse_x,g_mouse_y,g_mouse_pressed&&hover,g_mouse_down,g_mouse_released,hover,f.x,f.y,6*g_scale);
                if(move.move){f.x=std::clamp(move.x,0.0f,std::max(0.0f,g_screen_w-size));f.y=std::clamp(move.y,0.0f,std::max(0.0f,g_screen_h-size));if(g_mouse_released)g_folder_layout_dirty=true;}
                if(move.tap)g_open_folder=g_open_folder==f.id?"":f.id;
                draw_rect(f.x,f.y,size,size,kTitle);
                for(size_t i=0;i<std::min(size_t(4),members.size());++i){auto &ws=g_windows[members[i]];float x=f.x+5*g_scale+(i%2)*20*g_scale,y=f.y+5*g_scale+(i/2)*20*g_scale;
                    if(!ws.skin_icon.empty())draw_image(ws.skin_icon,x,y,18*g_scale,18*g_scale,kWhite);else draw_rect(x,y,18*g_scale,18*g_scale,kBtnHover);}
                if(g_open_folder!=f.id)continue;
                auto panel=folder_panel(f);draw_rect(panel.x,panel.y,panel.w,panel.h,kPanel);
                int &page=g_folder_pages[f.id];const int pages=(int(members.size())+8)/9;page=std::clamp(page,0,pages-1);
                const bool prev=mouse_in(panel.x,panel.y,40*g_scale,24*g_scale),next=mouse_in(panel.x+panel.w-40*g_scale,panel.y,40*g_scale,24*g_scale);
                if(widget_click("folder-prev",panel.x,panel.y,prev))page=std::max(0,page-1);
                if(widget_click("folder-next",panel.x+panel.w-40*g_scale,panel.y,next))page=std::min(pages-1,page+1);
                draw_string(panel.x+8*g_scale,panel.y+2*g_scale,"<",kText,0.7f);
                draw_string(panel.x+panel.w-25*g_scale,panel.y+2*g_scale,">",kText,0.7f);
                for(int i=page*9;i<std::min(int(members.size()),page*9+9);++i){auto id=members[i];auto &ws=g_windows[id];int slot=i-page*9;
                    float x=panel.x+8*g_scale+(slot%3)*64*g_scale,y=panel.y+28*g_scale+(slot/3)*64*g_scale;
                    bool hit=mouse_in(x,y,56*g_scale,56*g_scale);if(hit)g_hover_gui=true;
                    auto &gesture=g_member_gestures[id];
                    auto r=gesture.update(g_mouse_x,g_mouse_y,g_mouse_pressed&&hit,g_mouse_down,g_mouse_released,hit,x,y,8*g_scale);
                    if(r.move){g_member_drag=id;draw_rect(r.x,r.y,48*g_scale,48*g_scale,kBtnHover);
                        if(g_mouse_released&&!panel.contains(g_mouse_x,g_mouse_y)){
                            ws.x=std::clamp(r.x,0.0f,std::max(0.0f,g_screen_w-ws.icon_size));ws.y=std::clamp(r.y,0.0f,std::max(0.0f,g_screen_h-ws.icon_size));ws.collapsed=true;
                            f.members.erase(std::remove(f.members.begin(),f.members.end(),id),f.members.end());g_open_folder.clear();g_folder_layout_dirty=true;
                        }}
                    if(r.tap){ws.collapsed=false;ws.order=++g_next_window_order;g_open_folder.clear();}
                    if(!ws.skin_icon.empty())draw_image(ws.skin_icon,x+8*g_scale,y,40*g_scale,40*g_scale,kWhite);else draw_rect(x+8*g_scale,y,40*g_scale,40*g_scale,kBtn);
                    std::string label=ws.title.empty()?id:ws.title;
                    if(label.size()>24){size_t n=0;while(n<label.size()){size_t next=n+utf8_next(label.c_str()+n);if(next>24)break;n=next;}label.resize(n);}
                    draw_string(x,y+42*g_scale,label.c_str(),kText,0.42f);
                }
            }
            g_render_owner.clear();
            if(!g_mouse_down)g_member_drag.clear();
            for(auto it=g_folders.begin();it!=g_folders.end();)if(it->members.size()<2)it=g_folders.erase(it);else ++it;
            if(g_mouse_released){g_merge_target.clear();g_merge_source.clear();}
            if(!g_mouse_down)save_layout();
            g_recording=false;flush_cmds();
        }
        void draw_virtual_keyboard();

        bool point_in_gui(const float x, const float y) {
            if (x == -FLT_MAX || y == -FLT_MAX) return false;
            if(!folder_hit(x,y).empty())return true;
            if (g_edit.active && !ime::supported() && !kbd::supported()) {
                const float sw = g_screen_w > 0 ? static_cast<float>(g_screen_w) : 0.0f;
                const float sh = g_screen_h > 0 ? static_cast<float>(g_screen_h) : 0.0f;
                if (keyboard_contains(x, y, g_scale, sw, sh)) return true;
            }
            for (const auto &kv: g_windows) {
                const WindowState &ws = kv.second;
                if (!ws.visible||grouped_icon(kv.first)) continue;
                const bool compact = ws.collapsed;
                const float hit_w = compact ? (ws.skin_icon.empty()?title_height():ws.icon_size) : ws.w;
                const float body_h = compact ? hit_w : ws.collapsed ? title_height() : ws.h;
                if (x >= ws.x && x <= ws.x + hit_w && y >= ws.y && y <= ws.y + body_h) return true;
            }
            return false;
        }

        // ====================================================================
        // 绘制单个 Mod 的窗口
        // ====================================================================

        void end_scroll();
        void draw_mod_gui(lua_mod_handle_t *h) {
            if (!h || h->gui_ref == LUA_NOREF) return;

            WindowState &ws = g_windows[h->mod_id];
            g_style_window = &ws;
            g_render_owner=h->mod_id;

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

            if (ws.collapsed) {
                const float size = ws.skin_icon.empty()?title_height():ws.icon_size;
                const bool hit=mouse_in(ws.x,ws.y,size,size);
                const bool can_press=g_drag_id.empty() || g_drag_id==h->mod_id;
                if(hit && g_mouse_pressed && can_press)g_drag_id=h->mod_id;
                const auto gesture=ws.icon_gesture.update(g_mouse_x,g_mouse_y,
                    g_mouse_pressed && can_press,g_mouse_down,g_mouse_released,hit,
                    ws.x,ws.y,6.0f*g_scale);
                if(gesture.move){
                    ws.x=std::max(0.0f,std::min(gesture.x,std::max(0.0f,g_screen_w-size)));
                    ws.y=std::max(0.0f,std::min(gesture.y,std::max(0.0f,g_screen_h-size)));
                }
                const bool hovered=mouse_in(ws.x,ws.y,size,size);
                if(hovered || ws.icon_gesture.active)g_hover_gui=true;
                bool custom_icon=false;
                if(!ws.skin_icon.empty()){draw_image(ws.skin_icon,ws.x,ws.y,size,size,kWhite);int iw=0,ih=0;custom_icon=images::status(ws.skin_icon,iw,ih)=="ready";}
                if(!custom_icon)draw_rect(ws.x, ws.y, size, size, hovered ? kBtnHover : kTitle);
                if(!custom_icon)draw_string(ws.x + (size - text_width(ws.compact_label.c_str())) * 0.5f,
                            ws.y + (size - line_height()) * 0.5f,
                            ws.compact_label.empty()?"M":ws.compact_label.c_str(), kWhite, 1.0f);
                if(gesture.move){track_merge(h->mod_id);if(g_mouse_released)g_folder_layout_dirty=true;}
                if(!g_merge_target.empty()&&g_merge_source==h->mod_id)draw_string(ws.x,ws.y-20*g_scale,"+",kWhite,0.7f);
                if (gesture.tap) ws.collapsed = false;
                return;
            }
            const float p = pad();
            const float th = title_height();
            const float body_h = ws.collapsed ? th : ws.h;

            bool custom_panel=false;
            if(!ws.skin_panel.empty()) {
                draw_image(ws.skin_panel,ws.x,ws.y,ws.w,body_h,kWhite,0,0,0,0,ws.skin_border,ws.skin_edge);
                int iw=0,ih=0;custom_panel=images::status(ws.skin_panel,iw,ih)=="ready";
            }
            if(!custom_panel) {
                draw_rect(ws.x - 2.0f, ws.y - 2.0f, ws.w + 4.0f, body_h + 4.0f, kBorder);
                draw_rect(ws.x, ws.y, ws.w, body_h, kPanel);
                draw_rect(ws.x, ws.y, ws.w, th, kTitle);
                // DrawInvBG has fixed corners: a 1px rectangle produces a thick black bar.
            }

            // 收缩按钮（标题栏左端）：展开显示 v，收起显示 >
            const float a = th - 8.0f * g_scale;
            const float ax = ws.x + 4.0f * g_scale;
            const float ay = ws.y + (th - a) * 0.5f;
            const bool arrow_hovered = mouse_in(ax, ay, a, a);
            if (arrow_hovered) g_hover_gui = true;
            if (widget_click("window-collapse",ax,ay,arrow_hovered)) ws.collapsed = !ws.collapsed;
            if(ws.skin_panel.empty())draw_rect(ax, ay, a, a, arrow_hovered ? kBtnHover : kBtn);
            const char *arrow = ws.collapsed ? ">" : "v";
            draw_string(ax + (a - text_width(arrow)) * 0.5f, ay + (a - line_height()) * 0.5f, arrow,
                        kWhite, 1.0f);

            // 标题文本
            draw_string(ax + a + item_gap(), ws.y + (th - line_height()) * 0.5f, (ws.title.empty() ? h->mod_id.c_str() : ws.title.c_str()),
                        custom_panel ? themed_color(kText) : kWhite, 1.0f);

            // 标题栏拖动（避开收缩按钮）
            const bool title_hovered = mouse_in(ws.x, ws.y, ws.w, th) && !arrow_hovered;
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

            if(g_mouse_released&&g_drag_id==h->mod_id)g_folder_layout_dirty=true;

            // 收起：只画标题栏，不回调/不绘制内容
            if (ws.collapsed) {
                ws.h = th;
                return;
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
            while(!g_scroll_stack.empty())end_scroll();
            ws.initial_position=false;
            g_recording = false;
            g_current_mod.clear();
            g_ctx = nullptr;
            g_viewport={};
            if(ws.visible && !ws.collapsed)flush_cmds();else g_cmds.clear();

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
            if (widget_click(label,x,y,hovered)) {
                vkb_append(out);
                consumed = true;
            }
            draw_key_box(x, y, w, h, label, hovered);
        }

        /// 拼音输入法候选栏（PC：底部居中，数字键/点击选词，-/= 翻页）
        void draw_ime_bar() {
            if (g_edit.pinyin.empty()) return;
            constexpr size_t kPageSize = 9;
            const float h = 30.0f * g_scale;
            const float gap = 6.0f * g_scale;
            const float pad = 8.0f * g_scale;
            const size_t first = g_edit.page * kPageSize;
            const size_t last = std::min<size_t>(g_edit.cands.size(), first + kPageSize);
            const bool has_prev = g_edit.page > 0;
            const bool has_more = last < g_edit.cands.size();

            // 依次排布：模式 / ◀ / 候选… / 无候选 / ▶
            float total = 0.0f;
            auto chip_w = [&](const std::string &s) { return text_width(s.c_str()) + 16.0f * g_scale; };
            std::vector<std::string> labels;
            labels.push_back(g_edit.ime_on ? "中" : "英");
            if (has_prev) labels.push_back("◀");
            for (size_t i = first; i < last; ++i)
                labels.push_back(std::to_string(i - first + 1) + "." + g_edit.cands[i]);
            if (g_edit.cands.empty()) labels.push_back("无候选");
            if (has_more) labels.push_back("▶");
            for (auto &s: labels) total += chip_w(s) + gap;
            total -= gap;

            float x = (g_screen_w - total) * 0.5f;
            const float y = g_screen_h - h - 12.0f * g_scale;
            draw_rect(x - pad, y - pad, total + 2 * pad, h + 2 * pad, kPanel);

            auto chip = [&](const std::string &label, const char *id) -> bool {
                const float w = chip_w(label);
                const bool hover = mouse_in(x, y, w, h);
                if (hover) g_hover_gui = true;
                const bool clicked = widget_click(id, x, y, hover);
                draw_rect(x, y, w, h, hover ? kBtnHover : kBtn);
                draw_string(x + 8.0f * g_scale, y + (h - line_height()) * 0.5f, label.c_str(), kText, 1.0f);
                x += w + gap;
                return clicked;
            };

            chip(g_edit.ime_on ? "中" : "英", "ime-mode");
            if (has_prev && chip("◀", "ime-prev")) {
                if (g_edit.page > 0) --g_edit.page;
                return;
            }
            for (size_t i = first; i < last; ++i) {
                const std::string label = std::to_string(i - first + 1) + "." + g_edit.cands[i];
                const std::string id = "ime-cand" + std::to_string(i - first);
                if (chip(label, id.c_str())) {
                    g_edit.buf += g_edit.cands[i];
                    g_edit.pinyin.clear();
                    g_edit.cands.clear();
                    g_edit.page = 0;
                    return;
                }
            }
            if (g_edit.cands.empty()) chip("无候选", "ime-none");
            if (has_more && chip("▶", "ime-next")) {
                ++g_edit.page;
                return;
            }
        }

        void draw_virtual_keyboard() {
            // 几何与 keyboard_rect() 共用，保证热区与键盘一致
            const KeyboardRect k = keyboard_rect(g_scale, static_cast<float>(g_screen_w),
                                                 static_cast<float>(g_screen_h));
            const float key = k.key;
            const float wide = k.wide;
            const float gap = k.gap;
            const float pd = k.pad;
            const float kb_w = k.w;
            const float kb_h = k.h;
            const float ox = k.x;
            const float oy = k.y;
            const float inner = kb_w - 2.0f * pd;

            // 各行内容宽度（用于行内居中）
            const float w_qwe = 10.0f * key + 9.0f * gap;
            const float w_asd = 9.0f * key + 8.0f * gap;
            const float w_fun = wide + gap + 7.0f * key + 6.0f * gap + gap + wide;
            const float w_spc = 6.0f * key + gap + 3.0f * key;

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
                    if (widget_click("kb-shift",x,y,hv)) g_edit.shift = !g_edit.shift;
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
                    if (widget_click("kb-delete",x,y,hv) && !g_edit.buf.empty()) g_edit.buf.pop_back();
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
                    if (widget_click("kb-space",x,y,hv)) vkb_append(' ');
                    draw_key_box(x, y, space_w, key, "space", hv);
                    x += space_w + gap;
                }
                {
                    const float done_w = key * 3.0f;
                    const bool hv = mouse_in(x, y, done_w, key);
                    if (hv) g_hover_gui = true;
                    if (widget_click("kb-done",x,y,hv)) g_edit.active = false;
                    draw_key_box(x, y, done_w, key, "done", hv);
                }
            }
            (void) consumed;
        }

        // ====================================================================
        // Lua GUI 函数
        // ====================================================================

        int l_gui_begin_scroll(lua_State *L){
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            float cw=luaL_checknumber(L,6),ch=luaL_checknumber(L,7);
            if(!g_ctx||g_scroll_stack.size()>=8||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)||!std::isfinite(cw)||!std::isfinite(ch)||w<=0||h<=0||cw<0||ch<0)return luaL_error(L,"invalid scroll viewport");
            const std::string key=g_current_mod+"#"+id;
            if(!g_scrolls.count(key)&&g_scrolls.size()>=4096)return luaL_error(L,"scroll ID limit exceeded");
            auto &m=g_scrolls[key];
            Viewport bounds{true,g_ctx->ox+x,g_ctx->oy+y,w,h};
            float mx=std::max(0.0f,cw-w),my=std::max(0.0f,ch-h);m.clamp(mx,my);
            bool hit=mouse_in(bounds.x,bounds.y,w,h);
            if(hit)g_hover_gui=true;
            if(hit&&g_mouse_pressed&&!g_active_scroll.empty()&&!g_scroll_stack.empty()){
                auto old=g_scrolls.find(g_active_scroll);
                if(old!=g_scrolls.end()&&!old->second.bar_x&&!old->second.bar_y){old->second.active=false;g_active_scroll.clear();}
            }
            if(hit&&g_mouse_pressed&&g_active_scroll.empty()){
                g_active_scroll=key;m.begin(g_mouse_x,g_mouse_y,mx>0&&g_mouse_y>=bounds.y+h-12*g_scale,my>0&&g_mouse_x>=bounds.x+w-12*g_scale);
                if(m.bar_x||m.bar_y)g_click_state.armed=key+"#bar";
            }
            if(g_active_scroll==key){
                if(g_mouse_down||g_mouse_released)m.move(g_mouse_x,g_mouse_y,mx,my,
                    w-std::min(w,std::max(16*g_scale,cw>0?w*w/cw:w)),
                    h-std::min(h,std::max(16*g_scale,ch>0?h*h/ch:h)),6*g_scale);
                if(m.dragged){g_scroll_drag=true;g_click_state.armed=key+"#scroll";}
                if(!g_mouse_down){m.active=false;g_active_scroll.clear();}
            }
            if(hit&&!g_wheel_used&&g_wheel_delta){
                if(my>0)m.y-=g_wheel_delta*0.5f;else m.x-=g_wheel_delta*0.5f;
                m.clamp(mx,my);g_wheel_used=true;
            }
            ScrollFrame f;f.parent=g_ctx;f.old_clip=g_viewport;f.old_drag=g_scroll_drag;f.motion=&m;f.bounds=bounds;f.content_w=cw;f.content_h=ch;
            f.child.ox=bounds.x-m.x;f.child.oy=bounds.y-m.y;f.child.cw=cw;
            g_scroll_stack.push_back(f);g_ctx=&g_scroll_stack.back().child;
            g_viewport=bounds;if(f.old_clip.enabled){float px=bounds.x,py=bounds.y,pw=w,ph=h;f.old_clip.crop(px,py,pw,ph);g_viewport={true,px,py,std::max(0.0f,pw),std::max(0.0f,ph)};}
            lua_pushnumber(L,m.x);lua_pushnumber(L,m.y);lua_pushnumber(L,w);lua_pushnumber(L,h);return 4;
        }
        void end_scroll(){
            if(g_scroll_stack.empty())return;auto f=g_scroll_stack.back();g_scroll_stack.pop_back();g_ctx=f.parent;g_viewport=f.old_clip;
            float track=std::max(3.0f,5*g_scale);auto &m=*f.motion;
            if(f.content_h>f.bounds.h){float thumb=std::max(16*g_scale,f.bounds.h*f.bounds.h/f.content_h);thumb=std::min(thumb,f.bounds.h);
                float y=f.bounds.y+(f.bounds.h-thumb)*m.y/(f.content_h-f.bounds.h);
                draw_rect(f.bounds.x+f.bounds.w-track,y,track,thumb,kBorder);}
            if(f.content_w>f.bounds.w){float thumb=std::max(16*g_scale,f.bounds.w*f.bounds.w/f.content_w);thumb=std::min(thumb,f.bounds.w);
                float x=f.bounds.x+(f.bounds.w-thumb)*m.x/(f.content_w-f.bounds.w);
                draw_rect(x,f.bounds.y+f.bounds.h-track,thumb,track,kBorder);}
            // Keep click cancellation for the release frame, including siblings.
        }
        int l_gui_end_scroll(lua_State *){end_scroll();return 0;}
        int l_gui_scroll_to(lua_State *L){
            const char *id=luaL_checkstring(L,1);float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3);
            if(!std::isfinite(x)||!std::isfinite(y))return luaL_error(L,"invalid scroll offset");
            const std::string key=g_current_mod+"#"+id;
            if(!g_scrolls.count(key)&&g_scrolls.size()>=4096)return luaL_error(L,"scroll ID limit exceeded");
            auto &m=g_scrolls[key];m.x=std::max(0.0f,x);m.y=std::max(0.0f,y);return 0;
        }
        int l_gui_image_cache_stats(lua_State *L){size_t used,limit,entries;uint64_t evictions;images::stats(used,limit,entries,evictions);lua_pushinteger(L,used);lua_pushinteger(L,limit);lua_pushinteger(L,entries);lua_pushinteger(L,evictions);return 4;}
        Color lua_color(lua_State *L,int index);
        int l_gui_text(lua_State *L) {
            const char *s = luaL_checkstring(L, 1);
            if (!g_ctx) return 0;
            float x, y;
            const float w = text_width(s);
            const float h = line_height();
            place(x, y, w, h);
            Color color=lua_isnoneornil(L,2)?themed_color(kText):lua_color(L,2);
            draw_string(x, y, s, color, 1.0f);
            return 0;
        }

        int l_gui_text_wrapped(lua_State *L) {
            const char *s = luaL_checkstring(L, 1);
            Color color=lua_isnoneornil(L,2)?themed_color(kText):lua_color(L,2);
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
                    draw_string(cur_x, cur_y, line.c_str(), color, 1.0f);
                    cur_y += line_height();
                    cur_x = x;
                    line_w = 0.0f;
                    line.clear();
                }
                line += ch;
                line_w += cw;
                p += n;
            }
            if (!line.empty()) draw_string(cur_x, cur_y, line.c_str(), color, 1.0f);
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
            clicked=widget_click(label,x,y,hovered);
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);                                   // 按钮边框
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1,
                      hovered ? (g_mouse_down ? kBtnActive : kBtnHover) : kBtn);
            draw_string(x + 12.0f * g_scale, y + (h - line_height()) * 0.5f, label, kText, 1.0f);
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
            if (widget_click(label,x,y,hovered)) value = !value;

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
            draw_string(x + (w - text_width(ptxt)) * 0.5f, y + (h - line_height()) * 0.5f, ptxt, kText, 1.0f);
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
            if (widget_click(label,x,y,hovered)) open = !open;
            g_headers[id] = open;
            const float g1 = 1.0f * g_scale;
            draw_rect(x, y, w, h, kTrackEdge);
            draw_rect(x + g1, y + g1, w - 2 * g1, h - 2 * g1, hovered ? kBtnHover : kBtn);
            const std::string text = (open ? "v " : "> ") + std::string(label);
            draw_string(x + 8.0f * g_scale, y + (h - line_height()) * 0.5f, text.c_str(), kText, 1.0f);
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
                if(g_click_state.armed.empty())g_click_state.armed=g_render_owner+"#input-focus";
                g_edit.active = true;
                g_edit.owner = id;
                g_edit.buf = value;
                if (ime::supported()) {
                    ime::open(value);            // Android：弹出系统键盘（支持中文输入法）
                } else if (kbd::supported()) {
                    kbd::reset();
                }
            }

            const std::string shown = editing ? (g_edit.buf + g_edit.pinyin + "|") : std::string(value[0] ? value : "(tap to edit)");
            draw_rect(x, y, w, h, editing ? kBtnActive : (hovered ? kBtnHover : kBtn));
            draw_string(x + 8.0f * g_scale, y + (h - line_height()) * 0.5f, shown.c_str(), kText, 1.0f);

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
                if(!std::isfinite(x)||!std::isfinite(y))return luaL_error(L,"invalid window position");
                auto saved=g_saved_positions.find(g_current_mod);
                if(ws->initial_position&&saved!=g_saved_positions.end()){ws->x=saved->second.first;ws->y=saved->second.second;}
                else{ws->x=x;ws->y=y;}
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

        /// mod.gui.set_collapsed(v) -> v
        int l_gui_set_collapsed(lua_State *L) {
            const bool v = lua_toboolean(L, 1) != 0;
            if (WindowState *ws = current_window()) ws->collapsed = v;
            lua_pushboolean(L, v);
            return 1;
        }

        /// mod.gui.is_collapsed() -> bool
        int l_gui_is_collapsed(lua_State *L) {
            WindowState *ws = current_window();
            lua_pushboolean(L, ws && ws->collapsed);
            return 1;
        }

        /// mod.gui.toggle_collapsed() —— 收起/展开
        int l_gui_toggle_collapsed(lua_State *) {
            if (WindowState *ws = current_window()) ws->collapsed = !ws->collapsed;
            return 0;
        }

        Color lua_color(lua_State *L, int index) {
            luaL_checktype(L, index, LUA_TTABLE);
            index=lua_absindex(L,index);const char *names[]={"r","g","b","a"};
            int values[4] = {255, 255, 255, 255};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, index, i + 1);
                if(lua_isnil(L,-1)){lua_pop(L,1);lua_getfield(L,index,names[i]);}
                if (!lua_isnil(L, -1)) {
                    const lua_Integer v = luaL_checkinteger(L, -1);
                    values[i] = v < 0 ? 0 : v > 255 ? 255 : static_cast<int>(v);
                }
                lua_pop(L, 1);
            }
            Color c; make_color(c, values[0], values[1], values[2], values[3]); return c;
        }
        int l_gui_set_style(lua_State *L) {
            luaL_checktype(L, 1, LUA_TTABLE);
            WindowState *ws = current_window();
            if (!ws) return 0;
            const char *keys[] = {"panel", "title", "button", "hover", "active", "border", "text"};
            for (const char *key : keys) {
                lua_getfield(L, 1, key);
                if (!lua_isnil(L, -1)) ws->colors[key] = lua_color(L, lua_gettop(L));
                lua_pop(L, 1);
            }
            return 0;
        }
        // Absolute positions relative to the window content area, in screen pixels.
        int l_gui_rect(lua_State *L) {
            float x=luaL_checknumber(L,1), y=luaL_checknumber(L,2);
            float w=luaL_checknumber(L,3), h=luaL_checknumber(L,4);
            Color c=lua_color(L,5);
            if (g_ctx && w>0 && h>0) draw_rect(g_ctx->ox+x,g_ctx->oy+y,w,h,c);
            return 0;
        }
        int l_gui_text_at(lua_State *L) {
            float x=luaL_checknumber(L,1), y=luaL_checknumber(L,2);
            const char *text=luaL_checkstring(L,3);
            Color c=lua_color(L,4);
            float scale=luaL_optnumber(L,5,1.0f);
            if (g_ctx && scale>0 && scale<=4) draw_string(g_ctx->ox+x,g_ctx->oy+y,text,c,scale);
            return 0;
        }
        int l_gui_button_at(lua_State *L) {
            float x=luaL_checknumber(L,1), y=luaL_checknumber(L,2);
            float w=luaL_checknumber(L,3), h=luaL_checknumber(L,4);
            const char *text=luaL_checkstring(L,5);
            if (!g_ctx || w<=0 || h<=0) { lua_pushboolean(L,false); return 1; }
            x+=g_ctx->ox; y+=g_ctx->oy;
            const bool hover=mouse_in(x,y,w,h);
            if (hover) g_hover_gui=true;
            draw_rect(x,y,w,h,hover ? kBtnHover : kBtn);
            draw_string(x+(w-text_width(text))*0.5f,y+(h-line_height())*0.5f,text,kText);
            lua_pushboolean(L,widget_click(text,x,y,hover)); return 1;
        }
        int l_gui_set_content_height(lua_State *L) {
            const float height=luaL_checknumber(L,1);
            if (g_ctx && height>=0) {g_ctx->cy=height;g_ctx->prev_h=0;}
            return 0;
        }

        bool image_owned(const char *id) {return std::string(id).rfind(g_current_mod+"#",0)==0;}
        int l_gui_load_image(lua_State *L) {
            const char *path=luaL_checkstring(L,1);
            std::string root;
            for(auto &kv:core::lua_mod_handles)if(kv.second && kv.second->mod_id==g_current_mod)root=kv.second->private_dir;
            const auto id=root.empty()?std::string():images::request(g_current_mod,root,path);
            if(id.empty())lua_pushnil(L);else lua_pushlstring(L,id.data(),id.size());return 1;
        }
        int l_gui_release_image(lua_State *L) {
            const char *id=luaL_checkstring(L,1);if(image_owned(id))g_image_release_queue.push_back(id);return 0;
        }
        int l_gui_image_status(lua_State *L) {
            const char *id=luaL_checkstring(L,1);int w=0,h=0;
            const auto state=image_owned(id)?images::status(id,w,h):"invalid";
            lua_pushlstring(L,state.data(),state.size());lua_pushinteger(L,w);lua_pushinteger(L,h);return 3;
        }
        int l_gui_image(lua_State *L) {
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            Color c=lua_isnoneornil(L,6)?kWhite:lua_color(L,6);
            int src[4]={0,0,0,0};
            if(!lua_isnoneornil(L,7)){luaL_checktype(L,7,LUA_TTABLE);for(int i=0;i<4;++i){lua_rawgeti(L,7,i+1);src[i]=luaL_checkinteger(L,-1);lua_pop(L,1);}}
            if(g_ctx && image_owned(id))draw_image(id,g_ctx->ox+x,g_ctx->oy+y,w,h,c,src[0],src[1],src[2],src[3]);return 0;
        }
        int l_gui_image_fit(lua_State *L) {
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            const char *mode=luaL_optstring(L,6,"contain");
            Color c=lua_isnoneornil(L,7)?kWhite:lua_color(L,7);
            const int fit=std::strcmp(mode,"cover")==0?-2:-1;
            if(g_ctx && image_owned(id))draw_image(id,g_ctx->ox+x,g_ctx->oy+y,w,h,c,0,0,0,0,fit);return 0;
        }
        int l_gui_nine_slice(lua_State *L) {
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            int border=luaL_checkinteger(L,6);Color c=lua_isnoneornil(L,7)?kWhite:lua_color(L,7);
            float edge=luaL_optnumber(L,8,0);
            if(g_ctx && image_owned(id) && border>0)draw_image(id,g_ctx->ox+x,g_ctx->oy+y,w,h,c,0,0,0,0,border,edge);return 0;
        }
        int l_gui_image_button(lua_State *L) {
            const char *id=luaL_checkstring(L,1);
            float x=luaL_checknumber(L,2),y=luaL_checknumber(L,3),w=luaL_checknumber(L,4),h=luaL_checknumber(L,5);
            const char *label=luaL_optstring(L,6,"");
            int border=luaL_optinteger(L,7,0);
            if(!g_ctx || !image_owned(id)||w<=0||h<=0){lua_pushboolean(L,false);return 1;}
            x+=g_ctx->ox;y+=g_ctx->oy;const bool hovered=mouse_in(x,y,w,h);
            if(hovered)g_hover_gui=true;
            Color tint;make_color(tint,hovered?255:235,hovered?255:235,hovered?255:235,255);
            draw_image(id,x,y,w,h,tint,0,0,0,0,border);
            if(*label){
                const float estimated=text_width(label);
                const float available=std::max(1.0f,w-16.0f*g_scale);
                const float font_scale=estimated>available?available/estimated:1.0f;
                draw_string(x+(w-estimated*font_scale)*0.5f,
                            y+(h-line_height()*font_scale)*0.5f,label,kText,font_scale);
            }
            lua_pushboolean(L,widget_click(label,x,y,hovered));return 1;
        }
        int l_gui_set_skin(lua_State *L) {
            const char *panel=luaL_checkstring(L,1),*icon=luaL_checkstring(L,3);
            const int border=luaL_checkinteger(L,2);float size=luaL_optnumber(L,4,48);float edge=luaL_optnumber(L,5,0);
            if(WindowState *ws=current_window()){
                if(image_owned(panel)&&image_owned(icon)&&border>0&&size>=24&&size<=128){
                    ws->skin_panel=panel;ws->skin_icon=icon;ws->skin_border=border;ws->skin_edge=edge;ws->icon_size=size;
                }
            }return 0;
        }

        // Optional title and compact launcher; unchanged defaults for other mods.
        int l_gui_set_title(lua_State *L) {
            const char *title = luaL_checkstring(L, 1);
            const char *icon = luaL_optstring(L, 2, "");
            if (WindowState *ws = current_window()) {
                ws->title = title;
                ws->compact_label = icon;
            }
            return 0;
        }

        const luaL_Reg gui_funcs[] = {
                {"begin_scroll", l_gui_begin_scroll},
                {"end_scroll", l_gui_end_scroll},
                {"scroll_to", l_gui_scroll_to},
                {"image_cache_stats", l_gui_image_cache_stats},
                {"load_image", l_gui_load_image},
                {"image_status", l_gui_image_status},
                {"release_image", l_gui_release_image},
                {"image", l_gui_image},
                {"nine_slice", l_gui_nine_slice},
                {"image_fit", l_gui_image_fit},
                {"image_button", l_gui_image_button},
                {"set_skin", l_gui_set_skin},
                {"set_style", l_gui_set_style},
                {"rect", l_gui_rect},
                {"text_at", l_gui_text_at},
                {"button_at", l_gui_button_at},
                {"set_content_height", l_gui_set_content_height},
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
                {"set_title", l_gui_set_title},
                {"set_pos", l_gui_set_pos},
                {"get_pos", l_gui_get_pos},
                {"set_width", l_gui_set_width},
                {"set_cursor", l_gui_set_cursor},
                {"set_visible", l_gui_set_visible},
                {"is_visible", l_gui_is_visible},
                {"show", l_gui_show},
                {"hide", l_gui_hide},
                {"set_collapsed", l_gui_set_collapsed},
                {"is_collapsed", l_gui_is_collapsed},
                {"toggle_collapsed", l_gui_toggle_collapsed},
                {nullptr, nullptr},
        };

        // ====================================================================
        // 帧钩子
        // ====================================================================

        patch_hook_id_t g_draw_hook = PATCH_HOOK_INVALID_ID;
        patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
        patch_hook_id_t g_pi_hook = PATCH_HOOK_INVALID_ID; // PlayerInput.Update（IME 启用用）
        patch_hook_id_t g_cursor_hook = PATCH_HOOK_INVALID_ID; // Main.DrawCursor（GUI 时接管光标）
        bool g_installed = false;
        bool g_in_render = false;
#if defined(__ANDROID__)
        patch_hook_id_t g_layer_hook = PATCH_HOOK_INVALID_ID;
        patch_hook_id_t g_cursor_list_hook = PATCH_HOOK_INVALID_ID;
        std::unordered_map<int, bool> g_captured_touches;
        bool cursor_captured(void *cursor) {
            const auto f=g_gfx.f_cursor_id;
            auto *id=f && cursor ? static_cast<int*>(patchlib_field_get_pointer(f,cursor)) : nullptr;
            return id && g_captured_touches.count(*id);
        }
        void release_finished_touches() {
            if(!g_gfx.f_cm_instance || !g_gfx.m_cm_get_num_cursors || !g_gfx.m_cm_get_cursor)return;
            void *instance=read_static_object(g_gfx.f_cm_instance);if(!instance)return;
            auto count=reinterpret_cast<FnGetNumCursors>(patchlib_method_get_pointer(g_gfx.m_cm_get_num_cursors));
            auto at=reinterpret_cast<FnGetCursor>(patchlib_method_get_pointer(g_gfx.m_cm_get_cursor));
            if(!count||!at)return;
            const auto f=g_gfx.f_cursor_id;
            for(int i=0,n=count(instance);i<n;++i){void *c=at(instance,i);if(!c)continue;
                auto *id=f?static_cast<int*>(patchlib_field_get_pointer(f,c)):nullptr;
                auto *down=static_cast<unsigned char*>(patchlib_field_get_pointer(g_gfx.f_cursor_down,c));
                if(id && down && !*down)g_captured_touches.erase(*id);
            }
        }
        bool capture_cursor(void *cursor) {
            if (!cursor || !g_gfx.f_cursor_pos || !g_gfx.f_cursor_down || !g_gfx.m_adjust_real_screen) return false;
            auto *position=static_cast<Vec2*>(patchlib_field_get_pointer(g_gfx.f_cursor_pos,cursor));
            auto *down=static_cast<unsigned char*>(patchlib_field_get_pointer(g_gfx.f_cursor_down,cursor));
            const auto id_field=g_gfx.f_cursor_id;
            auto *id=id_field ? static_cast<int*>(patchlib_field_get_pointer(id_field,cursor)) : nullptr;
            if (!position || !down || !id) return false;
            auto adjust=reinterpret_cast<FnAdjustRealScreenForUI>(patchlib_method_get_pointer(g_gfx.m_adjust_real_screen));
            if (!adjust) return false;
            const Vec2 pos=adjust(*position);
            // Once claimed, keep this finger captured through its release frame.
            if (*down && point_in_gui(pos.x,pos.y)) g_captured_touches[*id]=true;
            if (!g_captured_touches.count(*id)) return false;
            for (auto field : {g_gfx.f_cursor_ignore_world,g_gfx.f_cursor_ignore}) {
                if (field) {auto *v=static_cast<unsigned char*>(patchlib_field_get_pointer(field,cursor));if(v)*v=1;}
            }
            return true;
        }
        bool cursor_layer_prefix(patch_handle_t, void **args, const patch_method_signature_t *, void *) {
            return args && args[0] && capture_cursor(*static_cast<void**>(args[0]));
        }
        void cursor_list_postfix(patch_handle_t, void **, void *, const patch_method_signature_t *) {
            if (!g_gfx.f_cm_instance || !g_gfx.m_cm_get_num_cursors || !g_gfx.m_cm_get_cursor) return;
            void *instance=read_static_object(g_gfx.f_cm_instance);
            if (!instance) return;
            auto count=reinterpret_cast<FnGetNumCursors>(patchlib_method_get_pointer(g_gfx.m_cm_get_num_cursors));
            auto at=reinterpret_cast<FnGetCursor>(patchlib_method_get_pointer(g_gfx.m_cm_get_cursor));
            if (!count || !at) return;
            std::unordered_map<int,bool> live;
            const auto id_field=g_gfx.f_cursor_id;
            for(int i=0,n=count(instance);i<n;++i){void *c=at(instance,i);if(!c)continue;
                auto *id=id_field?static_cast<int*>(patchlib_field_get_pointer(id_field,c)):nullptr;
                if(id)live[*id]=true;
                capture_cursor(c);
            }
            // Cursor objects persist for the release frame. Prune only disappeared IDs.
            for(auto it=g_captured_touches.begin();it!=g_captured_touches.end();){
                if(!live.count(it->first))it=g_captured_touches.erase(it);else ++it;
            }
        }
#endif

        /// 当前指针是否落在任一已打开窗口内（用上一帧缓存的位置）
        bool pointer_over_windows() {
            if (g_windows.empty() && !g_edit.active) return false;
#if defined(__ANDROID__)
            if (!g_mouse_down && !g_prev_mouse_down) return false;
            return point_in_gui(g_mouse_x, g_mouse_y);
#else
            if (!g_gfx.usable || !g_gfx.a_mouse_x.valid() || !g_gfx.a_mouse_left.valid()) return false;
            const float mx = static_cast<float>(read_int(g_gfx.a_mouse_x, 0));
            const float my = static_cast<float>(read_int(g_gfx.a_mouse_y, 0));
            return point_in_gui(mx, my);
#endif
        }

        /// Main.Update 前缀：指针悬停在 GUI 上时屏蔽游戏鼠标，避免误触游戏操作
        bool update_prefix(patch_handle_t, void **, const patch_method_signature_t *, void *) {
#if defined(__ANDROID__)
            void *cm_instance = g_gfx.f_cm_instance ? read_static_object(g_gfx.f_cm_instance) : nullptr;
            if (cm_instance && g_gfx.m_cm_get_num_cursors && g_gfx.m_cm_get_cursor &&
                g_gfx.m_adjust_real_screen && g_gfx.f_cursor_pos && g_gfx.f_cursor_down) {
                auto fn_num_cursors = reinterpret_cast<FnGetNumCursors>(patchlib_method_get_pointer(g_gfx.m_cm_get_num_cursors));
                auto fn_get_cursor = reinterpret_cast<FnGetCursor>(patchlib_method_get_pointer(g_gfx.m_cm_get_cursor));
                auto fn_adjust = reinterpret_cast<FnAdjustRealScreenForUI>(patchlib_method_get_pointer(g_gfx.m_adjust_real_screen));

                if (fn_num_cursors && fn_get_cursor && fn_adjust) {
                    const int num = fn_num_cursors(cm_instance);
                    for (int i = 0; i < num; ++i) {
                        void *c = fn_get_cursor(cm_instance, i);
                        if (!c) continue;
                        auto *pos_ptr = static_cast<Vec2 *>(patchlib_field_get_pointer(g_gfx.f_cursor_pos, c));
                        auto *down_ptr = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_down, c));
                        if (!pos_ptr || !down_ptr || *down_ptr == 0) continue;

                        const Vec2 ui_pos = fn_adjust(*pos_ptr);
                        if (point_in_gui(ui_pos.x, ui_pos.y) || cursor_captured(c)) {
                            if (g_gfx.f_cursor_ignore_world) {
                                auto *p = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_ignore_world, c));
                                if (p) *p = 1;
                            }
                            if (g_gfx.f_cursor_ignore) {
                                auto *p = static_cast<unsigned char *>(patchlib_field_get_pointer(g_gfx.f_cursor_ignore, c));
                                if (p) *p = 1;
                            }
                            write_bool(g_gfx.a_mouse_left, false);
                            write_bool(g_gfx.a_mouse_right, false);
                        }
                    }
                }
            }
#endif
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
            std::vector<std::string> control_image_owners;
            controls::append_image_owners(control_image_owners);
            if (!any_gui) {clear_text_cache();
                if(control_image_owners.empty())images::clear();
                else{images::prune(control_image_owners);images::begin_frame();}
                g_windows.clear();g_headers.clear();g_edit=EditState{};ime::close();kbd::reset();g_scrolls.clear();g_active_scroll.clear();
                g_pointer_owner.clear();g_input_owner.clear();g_drag_id.clear();g_slider_active.clear();g_click_state.reset();g_render_owner.clear();g_mouse_down=g_prev_mouse_down=false;
#if defined(__ANDROID__)
                g_active_cursor_valid=false;g_captured_touches.clear();
#endif
                return;}

            // 启动画面（showSplash）阶段贴图/字体尚未就绪，此时绘制会导致
            // DrawInvBG / DrawBorderString 解引用空资源而崩溃，必须跳过。
            if (read_bool(g_gfx.a_show_splash, false)) return;

#if defined(__ANDROID__)
            struct UIRenderScope {
                ~UIRenderScope() {
                    if (g_gfx.m_setup_rendering_ui) {
                        unsigned char v = 0;
                        void *args[1] = {&v};
                        patchlib_method_invoke_args(g_gfx.m_setup_rendering_ui, nullptr, nullptr, args);
                    }
                }
            } ui_scope;
            if (g_gfx.m_setup_rendering_ui) {
                unsigned char v = 1;
                void *args[1] = {&v};
                patchlib_method_invoke_args(g_gfx.m_setup_rendering_ui, nullptr, nullptr, args);
            }
#endif

            std::vector<std::string> owners;
            for(const auto &kv:core::lua_mod_handles)if(kv.second && kv.second->gui_ref!=LUA_NOREF)owners.push_back(kv.second->mod_id);
            auto image_owners=owners;
            image_owners.insert(image_owners.end(),control_image_owners.begin(),control_image_owners.end());
            images::prune(image_owners);
            auto owner_alive=[&owners](const std::string &key){for(const auto &id:owners)if(key.rfind(id+"#",0)==0)return true;return false;};
            if(g_edit.active && !owner_alive(g_edit.owner)){g_edit=EditState{};ime::close();kbd::reset();}
            for(auto it=g_scrolls.begin();it!=g_scrolls.end();)if(!owner_alive(it->first))it=g_scrolls.erase(it);else ++it;
            if(!g_active_scroll.empty()&&!owner_alive(g_active_scroll))g_active_scroll.clear();
            for(auto it=g_headers.begin();it!=g_headers.end();){if(!owner_alive(it->first))it=g_headers.erase(it);else ++it;}
            for(auto it=g_member_gestures.begin();it!=g_member_gestures.end();)if(!window_live(it->first))it=g_member_gestures.erase(it);else ++it;
            for(auto it=g_windows.begin();it!=g_windows.end();){
                bool live=false;for(const auto &id:owners)if(id==it->first){live=true;break;}
                if(!live){g_position_applied.erase(it->first);it=g_windows.erase(it);}else ++it;
            }
            for(const auto &id:g_image_release_queue)images::release(id);
            g_image_release_queue.clear();
            update_screen_size();
            update_input();
            images::begin_frame();g_scroll_drag=false;g_viewport={};g_wheel_delta=read_int(g_gfx.a_scroll_wheel,0);g_wheel_used=false;
            g_hover_gui = false;

            patch_handle_t sb = read_static_object(g_gfx.f_sprite_batch);
            if (!sb || !patchlib_is_valid(sb)) return;

            g_sb = sb;
            sb_begin();

            std::vector<lua_mod_handle_t*> windows;
            for(auto &kv:core::lua_mod_handles)if(kv.second && kv.second->gui_ref!=LUA_NOREF){
                auto &ws=g_windows[kv.second->mod_id];if(ws.order==0)ws.order=++g_next_window_order;
                const float access=ws.collapsed?(ws.skin_icon.empty()?title_height():ws.icon_size):title_height();
                ws.x=std::max(0.0f,std::min(ws.x,std::max(0.0f,g_screen_w-access)));
                ws.y=std::max(0.0f,std::min(ws.y,std::max(0.0f,g_screen_h-access)));
                windows.push_back(kv.second);
            }
            if(!g_pointer_owner.empty() && g_pointer_owner!="#keyboard"&&g_pointer_owner.rfind("#folder:",0)!=0){
                auto it=g_windows.find(g_pointer_owner);
                if(it==g_windows.end() || !it->second.visible)g_pointer_owner.clear();
            }
            for(auto *h:windows){if(!g_position_applied[h->mod_id]){auto pos=g_saved_positions.find(h->mod_id);if(pos!=g_saved_positions.end()){g_windows[h->mod_id].x=pos->second.first;g_windows[h->mod_id].y=pos->second.second;}g_position_applied[h->mod_id]=true;}}
            g_input_owner=g_pointer_owner;
            if(g_input_owner.empty())g_input_owner=folder_hit(g_mouse_x,g_mouse_y);
            if(g_mouse_pressed&&g_input_owner.rfind("#folder:",0)==0)g_pointer_owner=g_input_owner;
            if(g_input_owner=="#keyboard" && !g_edit.active)g_input_owner.clear();
            if(g_edit.active && !ime::supported() && !kbd::supported() && keyboard_contains(g_mouse_x,g_mouse_y,g_scale,g_screen_w,g_screen_h))g_input_owner="#keyboard";
            if(g_input_owner.empty()){
                int top=-1;
                for(auto *h:windows){const auto &ws=g_windows[h->mod_id];if(!ws.visible||grouped_icon(h->mod_id))continue;
                    const bool compact=ws.collapsed;
                    const float w=compact?(ws.skin_icon.empty()?title_height():ws.icon_size):ws.w;
                    const float height=compact?w:ws.collapsed?title_height():ws.h;
                    if(g_mouse_x>=ws.x&&g_mouse_x<=ws.x+w&&g_mouse_y>=ws.y&&g_mouse_y<=ws.y+height&&ws.order>=top){top=ws.order;g_input_owner=h->mod_id;}
                }
                if(g_mouse_pressed && !g_input_owner.empty()){
                    g_pointer_owner=g_input_owner;g_windows[g_input_owner].order=++g_next_window_order;
                }
            }
            std::sort(windows.begin(),windows.end(),[](auto *a,auto *b){return g_windows[a->mod_id].order<g_windows[b->mod_id].order;});
            for(auto *h:windows){if(!grouped_icon(h->mod_id))draw_mod_gui(h);g_render_owner.clear();}
            draw_folders();
            if(!g_mouse_down){g_drag_id.clear();g_slider_active.clear();g_pointer_owner.clear();}

#if defined(__ANDROID__)
            if(!g_mouse_down)g_active_cursor_valid=false;
#endif

            g_style_window=nullptr;
            if (g_edit.active){
                if (ime::supported()){
                    // Android：轮询系统软键盘，取代自绘虚拟键盘
                    std::string t;
                    const int st = ime::poll(t);
                    if (st == 0) g_edit.buf = t;                                  // 仍在编辑
                    else if (st == 1 || st == 3) { g_edit.buf = t; g_edit.active = false; } // 完成/失焦：提交
                    else g_edit.active = false;                                    // 2 = 取消：丢弃
                } else if (kbd::supported()){
                    // PC：硬件键盘 -> 内置拼音输入法（单独按 Shift 切换中/英）
                    kbd::Event ev; kbd::poll(ev);
                    constexpr size_t kPageSize = 9;
                    auto refresh = [&]() {
                        g_edit.cands.clear();
                        g_edit.page = 0;
                        if (!g_edit.ime_on || g_edit.pinyin.empty()) return;
                        for (auto &c: pinyin::query(g_edit.pinyin, kPageSize * 10))
                            g_edit.cands.push_back(c.text);
                    };
                    auto commit = [&](const std::string &s) {
                        g_edit.buf += s;
                        g_edit.pinyin.clear();
                        g_edit.cands.clear();
                        g_edit.page = 0;
                    };
                    if (ev.toggle_ime) {
                        g_edit.ime_on = !g_edit.ime_on;
                        g_edit.pinyin.clear();
                        g_edit.cands.clear();
                        g_edit.page = 0;
                        LOG_INFO("[ime] 中/英: {}", g_edit.ime_on ? "中" : "英");
                    }
                    if (!ev.ime.empty()) g_edit.buf += ev.ime;
                    bool pinyin_dirty = false;
                    const bool paging = g_edit.ime_on && !g_edit.pinyin.empty();
                    for (char ch: ev.append) {
                        if (paging && (ch == '-' || ch == '_')) {                 // 上一页
                            if (g_edit.page > 0) --g_edit.page;
                            continue;
                        }
                        if (paging && (ch == '=' || ch == '+')) {                 // 下一页
                            if ((g_edit.page + 1) * kPageSize < g_edit.cands.size()) ++g_edit.page;
                            continue;
                        }
                        if (g_edit.ime_on && ch >= 'a' && ch <= 'z') {
                            g_edit.pinyin.push_back(ch);
                            pinyin_dirty = true;
                        } else if (g_edit.ime_on && ch >= 'A' && ch <= 'Z') {
                            g_edit.pinyin.push_back(static_cast<char>(ch - 'A' + 'a'));
                            pinyin_dirty = true;
                        } else if (g_edit.ime_on && ch >= '1' && ch <= '9' && !g_edit.cands.empty()) {
                            const size_t idx = g_edit.page * kPageSize + static_cast<size_t>(ch - '1');
                            if (idx < g_edit.cands.size()) commit(g_edit.cands[idx]);
                        } else if (g_edit.ime_on && ch == ' ' && !g_edit.pinyin.empty()) {
                            commit(g_edit.cands.empty() ? g_edit.pinyin : g_edit.cands[0]);
                        } else {
                            g_edit.buf.push_back(ch);
                        }
                    }
                    if (ev.backspace) {
                        if (g_edit.ime_on && !g_edit.pinyin.empty()) {
                            g_edit.pinyin.pop_back();
                            pinyin_dirty = true;
                        } else {
                            utf8_pop_back(g_edit.buf); // 按整个字符删（修中文删两下）
                        }
                    }
                    if (pinyin_dirty) refresh();
                    if (ev.enter) {
                        if (g_edit.ime_on && !g_edit.pinyin.empty())
                            commit(g_edit.cands.empty() ? g_edit.pinyin : g_edit.cands[0]);
                        else g_edit.active = false;
                    }
                    if (ev.escape) {
                        if (!g_edit.pinyin.empty()) {
                            g_edit.pinyin.clear();
                            g_edit.cands.clear();
                            g_edit.page = 0;
                        } else g_edit.active = false;
                    }
                } else {
                    // 兜底：自绘虚拟键盘
                    g_cmds.clear();g_recording=true;draw_virtual_keyboard();g_recording=false;flush_cmds();
                }
                if (!g_edit.active) { kbd::reset(); }
                if (g_edit.active && !g_edit.pinyin.empty()) {
                    g_cmds.clear();g_recording=true;draw_ime_bar();g_recording=false;flush_cmds();
                }
            }
            if(!g_mouse_down)g_active_scroll.clear();
            g_click_state.finish(g_mouse_down,g_mouse_released);

            sb_end();
            g_sb = nullptr;
#if defined(__ANDROID__)
            release_finished_touches();
#endif

            // 指针悬停在 GUI 上时，屏蔽游戏的鼠标左键，避免同时触发游戏操作
            if (g_hover_gui) {
                write_bool(g_gfx.a_mouse_left, false);
                write_bool(g_gfx.a_mouse_right, false);
            }
        }

        void render_guarded() {
            if (g_in_render) return;
            struct RenderGuard{~RenderGuard(){g_in_render=false;g_recording=false;g_ctx=nullptr;g_sb=nullptr;g_style_window=nullptr;g_current_mod.clear();g_render_owner.clear();g_cmds.clear();g_scroll_stack.clear();g_viewport={};g_scroll_drag=false;}} guard;
            g_in_render = true;
            try{render();}catch(...){
                LOG_ERROR("[gui] render exception; resetting frame state");
                if(g_sb){try{sb_end();}catch(...){}}
                g_click_state.reset();g_drag_id.clear();g_slider_active.clear();g_pointer_owner.clear();
            }
        }

        void frame_postfix(patch_handle_t, void **, void *, const patch_method_signature_t *) {
            render_guarded();
        }

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

        // Block mobile controller input at dispatch, before button callbacks run.
#if defined(__ANDROID__)
        const auto layer_type=patchlib_type_get_type("", "CursorInputLayer");
        const auto layer_update=layer_type ? patchlib_type_get_method_by_param_count(layer_type,"UpdateCursor",1) : nullptr;
        if(layer_update && patchlib_is_valid(layer_update))
            g_layer_hook=patchlib_install_prepost_hook(layer_update,cursor_layer_prefix,nullptr);
        const auto cm_type=patchlib_type_get_type("", "CursorManager");
        const auto list_update=cm_type ? patchlib_type_get_method_by_param_count(cm_type,"UpdateCursorList",0) : nullptr;
        if(list_update && patchlib_is_valid(list_update))
            g_cursor_list_hook=patchlib_install_prepost_hook(list_update,nullptr,cursor_list_postfix);
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
        g_image_release_queue.clear();
        g_scrolls.clear();g_scroll_stack.clear();g_active_scroll.clear();g_viewport={};
        g_style_window=nullptr;g_ctx=nullptr;g_sb=nullptr;g_recording=false;g_in_render=false;
        save_layout();g_folders.clear();g_open_folder.clear();g_member_gestures.clear();g_position_applied.clear();
        g_windows.clear();g_headers.clear();g_edit=EditState{};ime::close();kbd::reset();g_cmds.clear();g_next_window_order=0;
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
