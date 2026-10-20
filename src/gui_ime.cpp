/*******************************************************************************
 * LuaLoader - gui_ime：系统软键盘（Android IME）
 * Copyright (C) 2026 lzup333
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * 用 UnityEngine.TouchScreenKeyboard 弹出系统软键盘（含中文输入法），
 * 取代自绘的虚拟键盘。仅 Android 有效；其它平台为桩实现。
 *******************************************************************************/

#include "gui_ime.hpp"

#if defined(__ANDROID__)

#include <cstdlib>
#include <string>

#include "logger.hpp"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/string.h"
#include "tefkernel/patchlib/type.h"

namespace {

    patch_handle_t g_kb = PATCH_NULL; // TouchScreenKeyboard 实例

    patch_handle_t t_kb = PATCH_NULL;
    patch_handle_t m_open = PATCH_NULL;       // static Open(text, type, autocorrect, multiline, secure, alert, placeholder, limit)
    patch_handle_t m_get_text = PATCH_NULL;
    patch_handle_t m_get_status = PATCH_NULL; // Status: 0 Visible, 1 Done, 2 Canceled, 3 LostFocus
    patch_handle_t m_set_active = PATCH_NULL;
    patch_handle_t m_destroy = PATCH_NULL;
    patch_handle_t m_set_hide = PATCH_NULL;   // static set_hideInput(bool)

    bool g_resolved = false;

    void resolve() {
        if (g_resolved) return;
        g_resolved = true;

        t_kb = patchlib_type_get_type("UnityEngine", "TouchScreenKeyboard");
        if (!patchlib_is_valid(t_kb)) {
            LOG_WARN("[ime] UnityEngine.TouchScreenKeyboard 不可用");
            return;
        }
        m_open = patchlib_type_get_method_by_param_count(t_kb, "Open", 8);
        m_get_text = patchlib_type_get_method_by_param_count(t_kb, "get_text", 0);
        m_get_status = patchlib_type_get_method_by_param_count(t_kb, "get_status", 0);
        m_set_active = patchlib_type_get_method_by_param_count(t_kb, "set_active", 1);
        m_destroy = patchlib_type_get_method_by_param_count(t_kb, "Destroy", 0);
        m_set_hide = patchlib_type_get_method_by_param_count(t_kb, "set_hideInput", 1);
    }

    std::string managed_to_utf8(patch_handle_t str) {
        if (!patchlib_is_valid(str)) return {};
        char *c = patchlib_string_cstr(str);
        if (!c) return {};
        std::string s(c);
        std::free(c);
        return s;
    }

} // namespace

namespace lualoader::gui::ime {

    bool supported() {
        resolve();
        return patchlib_is_valid(m_open);
    }

    void open(const std::string &initial) {
        resolve();
        if (!patchlib_is_valid(m_open)) return;
        close();

        // 隐藏原生输入框（我们自绘文本）
        if (patchlib_is_valid(m_set_hide)) {
            bool hide = true;
            void *a[1] = {&hide};
            patchlib_method_invoke_args(m_set_hide, PATCH_NULL, NULL, a);
        }

        patch_handle_t text = patchlib_string_create(initial.c_str());
        patch_handle_t placeholder = patchlib_string_create("");
        int keyboard_type = 0; // TouchScreenKeyboardType.Default
        bool autocorrect = false, multiline = false, secure = false, alert = false;
        int char_limit = 0;
        void *args[8] = {&text, &keyboard_type, &autocorrect, &multiline,
                         &secure, &alert, &placeholder, &char_limit};

        patch_handle_t kb = PATCH_NULL;
        patchlib_method_invoke_args(m_open, PATCH_NULL, &kb, args); // static
        patchlib_free(text);
        patchlib_free(placeholder);

        if (patchlib_is_valid(kb)) {
            g_kb = kb;
            LOG_INFO("[ime] 系统软键盘已弹出");
        } else {
            LOG_WARN("[ime] TouchScreenKeyboard.Open 失败");
        }
    }

    void close() {
        resolve();
        /* g_kb 为 null 是正常状态（例如没有打开系统输入法）；
         * 不要用 patchlib_is_valid 做空值判断，否则每帧 render close 会刷 WARN。 */
        if (!g_kb) return;
        if (patchlib_is_valid(m_set_active)) {
            bool active = false;
            void *a[1] = {&active};
            patchlib_method_invoke_args(m_set_active, g_kb, NULL, a);
        }
        if (patchlib_is_valid(m_destroy))
            patchlib_method_invoke_args(m_destroy, g_kb, NULL, NULL);
        patchlib_free(g_kb);
        g_kb = PATCH_NULL;
    }

    int poll(std::string &text) {
        resolve();
        if (!patchlib_is_valid(g_kb)) return 1;

        if (patchlib_is_valid(m_get_text)) {
            patch_handle_t s = PATCH_NULL;
            patchlib_method_invoke_args(m_get_text, g_kb, &s, NULL);
            text = managed_to_utf8(s);
            if (patchlib_is_valid(s)) patchlib_free(s);
        }

        int status = 0;
        if (patchlib_is_valid(m_get_status))
            patchlib_method_invoke_args(m_get_status, g_kb, &status, NULL);

        if (status != 0) {
            close();
            return status;
        }
        return 0;
    }

} // namespace lualoader::gui::ime

#else // 非 Android：桩

namespace lualoader::gui::ime {
    bool supported() { return false; }
    void open(const std::string &) {}
    void close() {}
    int poll(std::string &) { return 1; }
} // namespace lualoader::gui::ime

#endif
