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
                if (a.field) patchlib_free(a.field);
                a.field = nullptr;
                const patch_handle_t prop = patchlib_type_get_property(type, name);
                if (prop && patchlib_is_valid(prop)) {
                    a.getter = patchlib_property_get_get_method(prop);
                    a.setter = patchlib_property_get_set_method(prop);
                }
                // prop 只用于取访问器方法，本身不需要长期保留。
                if (prop) patchlib_free(prop);
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
                const auto t_input=patchlib_type_get_type("Terraria.GameInput","PlayerInput");
                if(t_input)g_gfx.a_scroll_wheel=resolve_accessor(t_input,"ScrollWheelDelta");
                /* t_input 不能释放：桌面端 a_scroll_wheel 若解析成字段会依附 class。 */
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
            patchlib_free(t_sb);

            const patch_handle_t t_utils = patchlib_type_get_type("Terraria", "Utils");
            if (t_utils) {
                g_gfx.m_draw_inv_bg = get_method(t_utils, "DrawInvBG", 3);
                g_gfx.m_draw_border_string = get_method(t_utils, "DrawBorderString", 8);
            }
            patchlib_free(t_utils);

#if defined(__ANDROID__)
            const patch_handle_t t_runner = patchlib_type_get_type("", "XNAUnityRunner");
            if (t_runner) {
                g_gfx.m_setup_rendering_ui = get_method(t_runner, "SetupRenderingUI", 1);
                g_gfx.m_adjust_real_screen = get_method(t_runner, "AdjustRealScreenForUI", 1);
            }
            patchlib_free(t_runner);

            const patch_handle_t t_cm = patchlib_type_get_type("", "CursorManager");
            if (t_cm) {
                g_gfx.f_cm_instance = patchlib_type_get_field(t_cm, "Instance");
                g_gfx.m_cm_get_num_cursors = get_method(t_cm, "GetNumCursors", 0);
                g_gfx.m_cm_get_cursor = get_method(t_cm, "GetCursor", 1);
            }
            patchlib_free(t_cm);

            const patch_handle_t t_cursor = patchlib_type_get_type("", "Cursor");
            if (t_cursor) {
                g_gfx.f_cursor_pos = patchlib_type_get_field(t_cursor, "Position");
                g_gfx.f_cursor_down = patchlib_type_get_field(t_cursor, "Down");
                g_gfx.f_cursor_id = patchlib_type_get_field(t_cursor, "Id");
                g_gfx.f_cursor_ignore_world = patchlib_type_get_field(t_cursor, "IgnoreForWorld");
                g_gfx.f_cursor_ignore = patchlib_type_get_field(t_cursor, "Ignore");
            }
            patchlib_free(t_cursor);
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

        /// 释放 GUI 长期缓存的反射句柄（shutdown 时调用）。
        void release_gfx() {
            auto release_accessor = [](Accessor &a) {
                patchlib_free(a.field);
                patchlib_free(a.getter);
                patchlib_free(a.setter);
                a = Accessor{};
            };
            patchlib_free(g_gfx.t_main);
            patchlib_free(g_gfx.f_sprite_batch);
            patchlib_free(g_gfx.m_sb_begin);
            patchlib_free(g_gfx.m_sb_begin8);
            patchlib_free(g_gfx.m_sb_end);
            patchlib_free(g_gfx.m_draw_inv_bg);
            patchlib_free(g_gfx.m_draw_border_string);
            release_accessor(g_gfx.a_screen_w);
            release_accessor(g_gfx.a_screen_h);
            release_accessor(g_gfx.a_mouse_x);
            release_accessor(g_gfx.a_mouse_y);
            release_accessor(g_gfx.a_mouse_left);
            release_accessor(g_gfx.a_mouse_left_release);
            release_accessor(g_gfx.a_mouse_right);
            release_accessor(g_gfx.a_scroll_wheel);
            release_accessor(g_gfx.a_show_splash);
#if defined(__ANDROID__)
            patchlib_free(g_gfx.m_setup_rendering_ui);
            patchlib_free(g_gfx.m_adjust_real_screen);
            patchlib_free(g_gfx.f_cm_instance);
            patchlib_free(g_gfx.m_cm_get_num_cursors);
            patchlib_free(g_gfx.m_cm_get_cursor);
            patchlib_free(g_gfx.f_cursor_pos);
            patchlib_free(g_gfx.f_cursor_down);
            patchlib_free(g_gfx.f_cursor_id);
            patchlib_free(g_gfx.f_cursor_ignore_world);
            patchlib_free(g_gfx.f_cursor_ignore);
#endif
            g_gfx = Gfx{};
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
#if defined(_WIN32)
                // Windows/XNA: Begin(SpriteSortMode, BlendState)，第二个参数是
                // 引用类型，必须传 null 句柄，不能传 bool。
                int sort_mode = 0; // SpriteSortMode.Deferred
                patch_handle_t null_blend = nullptr;
                void *args[2] = {&sort_mode, &null_blend};
                patchlib_method_invoke_args(g_gfx.m_sb_begin, g_sb, nullptr, args);
#else
                int sort_mode = 0; // SpriteSortMode.Deferred
                unsigned char deferred = 1;
                void *args[2] = {&sort_mode, &deferred};
                patchlib_method_invoke_args(g_gfx.m_sb_begin, g_sb, nullptr, args);
#endif
            }
#if defined(_WIN32)
            else if (g_gfx.sb_begin_args == 8) {
                // Begin(SpriteSortMode, BlendState, SamplerState, DepthStencilState,
                //       RasterizerState, Effect, Matrix?, bool)
                int sort_mode = 0; // SpriteSortMode.Deferred
                patch_handle_t null_state = nullptr;
                NullableMat no_transform{};
                no_transform.has_value = 0;
                unsigned char deferred = 1;
                void *args[8] = {
                    &sort_mode, &null_state, &null_state, &null_state,
                    &null_state, &null_state, &no_transform, &deferred
                };
                patchlib_method_invoke_args(g_gfx.m_sb_begin, g_sb, nullptr, args);
            }
#endif
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
