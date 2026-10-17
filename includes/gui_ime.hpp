#pragma once
#include <string>

namespace lualoader::gui::ime {

    /// 当前平台是否支持系统软键盘（仅 Android）。非 Android 返回 false。
    bool supported();

    /// 弹出系统软键盘（Android IME，可用中文输入法），初始文本 initial。
    void open(const std::string &initial);

    /// 关闭系统软键盘。
    void close();

    /// 轮询系统键盘状态，把当前文本写入 text。
    /// 返回状态码：0=仍在编辑，1=完成(Done)，2=取消(Canceled)，3=失焦(LostFocus)。
    /// 结束时内部已 close()。非 Android 恒返回 1。
    int poll(std::string &text);

} // namespace lualoader::gui::ime
