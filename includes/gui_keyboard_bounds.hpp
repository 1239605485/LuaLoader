#pragma once
#include <algorithm>
namespace lualoader::gui {
// 虚拟键盘的几何参数必须与 draw_virtual_keyboard() 完全一致，否则
// “键盘热区 / 输入屏蔽区域”会与实际绘制的键盘错位。
// 注：按键偏小，故整体用较大的尺寸（key=40/gap=6）。
struct KeyboardRect {
    float x, y, w, h;   // 面板矩形
    float key, gap, pad, wide;
};
inline KeyboardRect keyboard_rect(float scale, float screen_w, float screen_h) {
    KeyboardRect k{};
    k.key = 40.0f * scale;
    k.wide = 56.0f * scale;
    k.gap = 6.0f * scale;
    k.pad = 10.0f * scale;
    // 最宽行：数字/qwerty 行(10 键) 或 功能行(shift+7键+del)
    float inner = 10.0f * k.key + 9.0f * k.gap;
    const float fun = 2.0f * k.wide + 7.0f * k.key + 8.0f * k.gap;
    if (fun > inner) inner = fun;
    k.w = inner + 2.0f * k.pad;
    k.h = 5.0f * k.key + 4.0f * k.gap + 2.0f * k.pad;
    k.x = std::max(0.0f, (screen_w - k.w) * 0.5f);
    k.y = std::max(0.0f, screen_h - k.h - 10.0f * scale);
    return k;
}
inline bool keyboard_contains(float x, float y, float scale, float screen_w, float screen_h) {
    const KeyboardRect k = keyboard_rect(scale, screen_w, screen_h);
    return x >= k.x && x <= k.x + k.w && y >= k.y && y <= k.y + k.h;
}
}
