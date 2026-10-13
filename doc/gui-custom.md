# Lua 模组自定义界面（1.8.0-ui-test）

在 `on_gui()` 回调内调用。每个模组有独立窗口与配色，所有坐标以窗口内容区左上角为原点，单位为屏幕像素。

```lua
function on_gui()
    local g = mod.gui
    g.set_title("任务手册", "册")
    g.set_width(300)
    g.set_style({panel={18,29,40,245}, title={29,57,69,255},
                 button={33,67,78,255}, hover={46,96,109,255},
                 border={9,19,27,255}, text={228,237,232,255}})
    g.rect(0,0,276,72,{24,48,59,255})
    g.text_at(12,12,"任务介绍",{232,220,165,255},1)
    if g.button_at(0,90,276,42,"查看任务") then
        -- 切换自己的页面状态
    end
    g.set_content_height(140)
end
```

- `set_style(table)`：panel/title/button/hover/active/border/text，各颜色为{R,G,B,A}，通道0~255，A可省略。只影响调用的模组。
- `rect(x,y,w,h,color)`：绘制游戏原生背景块（沿用DrawInvBG纹理，不是无纹理纯色矩形）。
- `text_at(x,y,text,color,scale=1)`：自定义位置、颜色、字号。
- `button_at(x,y,w,h,label)`：自定义位置和尺寸，松手命中时返回true。
- `set_content_height(h)`：使用自定义布局时声明内容高度，避免窗口高度不足。
- `set_title(title,icon)`：标题与收起按钮文字，icon建议一个字；收起外观仍使用游戏原生背景。

已有 `set_pos/set_width/set_cursor/set_visible/set_collapsed` 可继续用。移动位置只需首次设置，否则每帧重设会阻止拖动。显式定位控件需自己避免越出窗口，当前没有裁剪API、图片API、独立窗口创建API或圆角参数。

移动触摸拦截：CursorManager.UpdateCursorList 后标记GUI触摸，CursorInputLayer.UpdateCursor前跳过该手指的游戏输入分发，持续到松手。其他手指在界面外保持游戏输入。日志记录两个hook ID；部分底层路径若绕过该分发层仍需实机定位。

升级一次加载器后，后续模组改样式和布局只需改Lua。原有模组接口兼容。
