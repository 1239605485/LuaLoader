/*******************************************************************************
 * LuaLoader - gui_keyboard：物理键盘文本输入（PC / 有硬件键盘的 Android）
 * Copyright (C) 2026 lzup333
 *
 * 通过 Terraria 的 PlayerInput.GetPressedKeys() 读取物理键盘按下的键，
 * 自行做边沿检测并映射为字符，喂给 GUI 输入框。桌面端取代自绘虚拟键盘。
 * 输入期间把 PlayerInput.WritingText 置真，令游戏清空热键输入，避免误触。
 *******************************************************************************/

#include "gui_keyboard.hpp"

#include <set>
#include <string>

#include "logger.hpp"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/patchlib/struct/list.h"
#include "tefkernel/patchlib/type.h"

namespace {

    patch_handle_t t_player_input = PATCH_NULL;
    patch_handle_t m_get_pressed = PATCH_NULL; // static List<Keys> GetPressedKeys()
    patch_handle_t f_writing_text = PATCH_NULL; // static bool WritingText
    patch_handle_t f_list_items = PATCH_NULL;   // List<Keys>._items
    // 输入法(IME)提交字符缓存：Main.keyInt / Main.keyCount
    patch_handle_t f_main_key_int = PATCH_NULL;
    patch_handle_t f_main_key_count = PATCH_NULL;

    bool g_resolved = false;
    std::set<int> g_prev_keys;
    bool g_shift_down = false;
    bool g_shift_used = false; // 本次按住 Shift 期间是否按过别的键

    void resolve() {
        if (g_resolved) return;
        g_resolved = true;
        t_player_input = patchlib_type_get_type("Terraria.GameInput", "PlayerInput");
        if (!patchlib_is_valid(t_player_input)) {
            LOG_WARN("[kbd] Terraria.GameInput.PlayerInput 不可用");
            return;
        }
        m_get_pressed = patchlib_type_get_method_by_param_count(t_player_input, "GetPressedKeys", 0);
        f_writing_text = patchlib_type_get_field(t_player_input, "WritingText");

        // 取 GetPressedKeys 返回类型 List<Keys> 的 _items 字段句柄。
        // 不用 patchlib_list_get_array：它内部用 il2cpp_object_get_class，桌面端对泛型实例会崩。
        if (patchlib_is_valid(m_get_pressed)) {
            patch_method_signature_t sig;
            if (patchlib_method_get_signature(m_get_pressed, &sig)) {
                if (patchlib_is_valid(sig.return_type_handle))
                    f_list_items = patchlib_type_get_field(sig.return_type_handle, "_items");
                patchlib_method_signature_free(&sig);
            }
        }
        LOG_INFO("[kbd] resolve: PlayerInput={} GetPressedKeys={} WritingText={} items={}",
                 t_player_input ? "ok" : "null",
                 patchlib_is_valid(m_get_pressed) ? "ok" : "null",
                 patchlib_is_valid(f_writing_text) ? "ok" : "null",
                 patchlib_is_valid(f_list_items) ? "ok" : "null");

        // 输入法(IME)提交字符：Main.keyInt / Main.keyCount
        patch_handle_t t_main = patchlib_type_get_type("Terraria", "Main");
        if (patchlib_is_valid(t_main)) {
            f_main_key_int = patchlib_type_get_field(t_main, "keyInt");
            f_main_key_count = patchlib_type_get_field(t_main, "keyCount");
            patchlib_free(t_main);
        }
        LOG_INFO("[kbd] resolve: Main.keyInt={} Main.keyCount={}",
                 patchlib_is_valid(f_main_key_int) ? "ok" : "null",
                 patchlib_is_valid(f_main_key_count) ? "ok" : "null");
    }

    // UTF-16 码元 -> UTF-8（BMP 足够，中文都在 BMP）
    void append_utf8(std::string &out, unsigned int cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // XNA/Microsoft.Xna.Framework.Input.Keys -> 字符（0 表示非可见字符）
    char map_key(const int k, const bool shift) {
        if (k >= 65 && k <= 90) return shift ? static_cast<char>(k) : static_cast<char>(k + 32); // A-Z
        if (k >= 48 && k <= 57) {                                                                 // D0-D9
            static const char *sym = ")!@#$%^&*(";
            return shift ? sym[k - 48] : static_cast<char>(k);
        }
        if (k >= 96 && k <= 105) return static_cast<char>('0' + (k - 96)); // NumPad0-9
        switch (k) {
            case 32: return ' ';                                             // Space
            case 189: return shift ? '_' : '-';   // OemMinus
            case 187: return shift ? '+' : '=';   // OemPlus
            case 219: return shift ? '{' : '[';   // OemOpenBrackets
            case 221: return shift ? '}' : ']';   // OemCloseBrackets
            case 220: return shift ? '|' : '\\';  // OemPipe
            case 186: return shift ? ':' : ';';   // OemSemicolon
            case 222: return shift ? '"' : '\'';  // OemQuotes
            case 188: return shift ? '<' : ',';   // OemComma
            case 190: return shift ? '>' : '.';   // OemPeriod
            case 191: return shift ? '?' : '/';   // OemQuestion
            case 192: return shift ? '~' : '`';   // OemTilde
            default: return 0;
        }
    }

} // namespace

namespace lualoader::gui::kbd {

    bool supported() {
        resolve();
        return patchlib_is_valid(m_get_pressed);
    }

    void set_text_mode(const bool on) {
        resolve();
        if (!patchlib_is_valid(f_writing_text)) return;
        // 注意：游戏每帧会把 WritingText 复位，这里每次都必须真写，不能靠缓存短路。
        bool value = on;
        patchlib_field_set_value(f_writing_text, PATCH_NULL, &value);
    }

    void reset() {
        g_prev_keys.clear();
        g_shift_down = false;
        g_shift_used = false;
    }

    void poll(Event &out) {
        resolve();
        if (!patchlib_is_valid(m_get_pressed)) return;

        patch_handle_t list = PATCH_NULL;
        if (!patchlib_method_invoke_args(m_get_pressed, PATCH_NULL, &list, NULL) ||
            !patchlib_is_valid(list))
            return;

        std::set<int> now;
        if (patchlib_is_valid(f_list_items)) {
            patch_handle_t arr = PATCH_NULL;
            patchlib_field_get_value(f_list_items, list, &arr); // List<Keys>._items
            if (patchlib_is_valid(arr)) {
                const size_t n = patchlib_array_length(arr);
                for (size_t i = 0; i < n; ++i) {
                    int32_t key = 0;
                    if (patchlib_array_at(arr, i, &key)) now.insert(key);
                }
                patchlib_free(arr);
            }
        }
        patchlib_free(list);

        // 输入法(IME)提交的字符：Main.keyInt[0..keyCount)（含中文与 ASCII），读完清零
        if (patchlib_is_valid(f_main_key_int) && patchlib_is_valid(f_main_key_count)) {
            int32_t count = 0;
            patchlib_field_get_value(f_main_key_count, PATCH_NULL, &count);
            if (count > 0) {
                patch_handle_t arr = PATCH_NULL;
                patchlib_field_get_value(f_main_key_int, PATCH_NULL, &arr);
                if (patchlib_is_valid(arr)) {
                    const size_t n = patchlib_array_length(arr);
                    const size_t lim = static_cast<size_t>(count) < n ? static_cast<size_t>(count) : n;
                    for (size_t i = 0; i < lim; ++i) {
                        int32_t code = 0;
                        if (patchlib_array_at(arr, i, &code) && code >= 32 && code != 127)
                            append_utf8(out.ime, static_cast<unsigned int>(code));
                    }
                    patchlib_free(arr);
                }
                int32_t zero = 0;
                patchlib_field_set_value(f_main_key_count, PATCH_NULL, &zero); // 消费掉，避免残留
            }
        }

        const bool shift = now.count(160) || now.count(161); // LeftShift / RightShift
        if (shift && !g_shift_down) g_shift_used = false;    // Shift 刚按下

        bool non_shift_new = false;
        for (const int k: now) {
            if (g_prev_keys.count(k)) continue; // 只处理“本帧新按下”
            if (k == 160 || k == 161) continue; // Shift 本身不算“配合其它键”
            non_shift_new = true;
            if (k == 8) { out.backspace = true; continue; }        // Back
            if (k == 13) { out.enter = true; continue; }           // Enter
            if (k == 27) { out.escape = true; continue; }          // Escape
            const char c = map_key(k, shift);
            if (c) out.append.push_back(c);
        }
        if (shift && non_shift_new) g_shift_used = true;
        // 单独按一下 Shift（按住期间没配合其它键）后松开 → 切换中/英
        if (!shift && g_shift_down && !g_shift_used) out.toggle_ime = true;
        g_shift_down = shift;

        g_prev_keys = std::move(now);
    }

} // namespace lualoader::gui::kbd
