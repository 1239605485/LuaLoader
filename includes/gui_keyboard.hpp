#pragma once
#include <string>

namespace lualoader::gui::kbd {

    /// 一帧内解析出的编辑动作
    struct Event {
        std::string append;      // 物理键盘本帧新输入的字符（UTF-8，通常为 ASCII）
        std::string ime;         // 输入法(IME)本帧提交的字符（含中文）
        bool backspace = false;
        bool enter = false;
        bool escape = false;
        bool toggle_ime = false; // 单独按一下 Shift（未配合其它键）→ 切换中/英
    };

    /// 能否读取物理键盘（Terraria 的 PlayerInput.GetPressedKeys 可用时为真）
    bool supported();

    /// 打开/关闭文本输入模式（PlayerInput.WritingText）：
    /// 为 true 时游戏会清空热键输入，避免打字时误触游戏操作。
    void set_text_mode(bool on);

    /// 清空按键边沿状态（开始/结束编辑时调用）
    void reset();

    /// 每帧轮询物理键盘，输出本帧新按下的字符 / 退格 / 回车 / ESC。
    void poll(Event &out);

} // namespace lualoader::gui::kbd
