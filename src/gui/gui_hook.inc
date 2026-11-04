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
            if (!any_gui) {clear_text_cache();images::clear();g_windows.clear();g_headers.clear();g_edit=EditState{};ime::close();kbd::reset();g_scrolls.clear();g_active_scroll.clear();
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
            images::prune(owners);
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
            // 桌面端 read_static_object 每次都会新建 GCHandle，必须在本帧结束时释放，
            // 否则渲染钩子每帧都会泄漏一个句柄（越玩越卡的主因之一）。
            struct StaticObjectGuard {
                patch_handle_t h;
                ~StaticObjectGuard() { patchlib_free(h); }
            } sb_guard{sb};
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

