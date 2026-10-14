# LuaLoader 原生 GUI（`mod.gui` / `mod.on_gui`）

> 1.7.0 起，LuaLoader 的界面由 **Dear ImGui** 改为**直接用游戏自带的 XNA / Unity 绘制接口**。
> 不再叠加独立渲染后端（OpenGL 叠加 / `eglSwapBuffers`），也不需要额外开关。

## 1. 概览

- 只要定义了全局函数 `on_gui`（即 `mod.on_gui`），加载器就会为该 Mod 创建一个**可拖动窗口**，每帧回调一次。
- 绘制全部走游戏的 `SpriteBatch` 与 `Terraria.Utils`，因此在
  - **PC**：Windows 走 **XNA**，其它平台走 **FNA**；
  - **PE（Android）**：走 Unity 上的 **XNA 兼容层**；
  三端表现一致。
- 文本：`Terraria.Utils.DrawBorderString`；面板/按钮/轨道背景：`Terraria.Utils.DrawInvBG`。
- 窗口内的所有坐标（包括自定义布局与图片）都以**窗口内容区左上角**为原点，单位为屏幕像素。

## 2. 窗口控制

| 函数 | 说明 |
|---|---|
| `mod.gui.set_pos(x, y)` | 设置窗口左上角位置（屏幕像素） |
| `mod.gui.get_pos()` → `x, y` | 取当前窗口位置 |
| `mod.gui.set_width(w)` | 设置窗口宽度（高度随内容自适应） |
| `mod.gui.set_visible(v)` → `v` | 显示 / 隐藏窗口 |
| `mod.gui.is_visible()` → `bool` | 当前是否显示 |
| `mod.gui.show()` / `mod.gui.hide()` | 显示 / 隐藏 |
| `mod.gui.set_collapsed(v)` → `v` | 收起 / 展开 |
| `mod.gui.is_collapsed()` → `bool` | 当前是否收起 |
| `mod.gui.toggle_collapsed()` | 切换收起 / 展开 |
| `mod.gui.set_title(title, icon?)` | 设置窗口标题；`icon` 建议一个字，作为收起入口 |
| `mod.gui.set_cursor(x, y)` | 移动布局光标（相对内容区左上角），之后控件从该处开始画 |
| `mod.gui.set_content_height(h)` | 使用自定义布局时声明内容高度，避免窗口高度不足 |

- 标题栏左端有一个箭头按钮：`v` 展开、`>` 收起（点击即切换，像 ImGui 的折叠）。
- 收起时若未设置 `icon`，只保留标题栏；若设置了 `icon`（且未使用图片皮肤），则收成一个
  可拖动的**紧凑入口方块**，点击展开。设置 `set_skin` 的图标后会以皮肤图片作为收起入口。
- 收起后不绘制内容也不回调 `on_gui`，可显著减少对视野的遮挡。
- 移动位置只需首次设置，否则每帧重设会阻止拖动。

## 3. 内置控件（即时模式）

控件按顺序自上而下排列；把返回值写回即可：

| 函数 | 返回值 | 说明 |
|---|---|---|
| `mod.gui.text(s)` | – | 一行文本 |
| `mod.gui.text_wrapped(s)` | – | 按窗口宽度自动换行 |
| `mod.gui.button(label)` | `bool` | 按钮；本帧被点击返回 true |
| `mod.gui.checkbox(label, v)` | `bool` | 复选框 |
| `mod.gui.slider(label, v, min, max)` | `number` | 滑条 |
| `mod.gui.input_text(label, v)` | `string` | 文本框（点击弹虚拟键盘） |
| `mod.gui.same_line()` | – | 下一个控件与上一个同行 |
| `mod.gui.separator()` / `mod.gui.spacing()` | – | 分隔线 / 空行 |
| `mod.gui.collapsing_header(label)` | `bool` | 可折叠标题 |
| `mod.gui.progress_bar(v)` | – | 进度条（0.0~1.0，带百分比） |

示例：

```lua
local speed, name, visible = 3.0, "", true

function mod.on_gui()
    mod.gui.set_visible(visible)         -- 由自己的标志位控制出现/消失
    mod.gui.text("我的 Mod")
    mod.gui.separator()
    speed = mod.gui.slider("速度", speed, 1.0, 10.0)
    name  = mod.gui.input_text("名字", name)
    if mod.gui.button("固定到左上角") then
        mod.gui.set_pos(20, 20)
    end
end
```

> 隐藏时**仍会每帧回调 `on_gui`**（允许在里面 `show()` 重新显示），但控件不绘制、
> 不响应点击，也不屏蔽游戏鼠标。可用热键 / 聊天指令改动自己的标志位后在 `on_gui` 里
> `set_visible(flag)`，即可实现“按需出现”。

## 4. 自定义布局与配色

不使用内置控件、自己摆放时，可用下面这些接口。`color` 均为 `{R,G,B,A}`，
通道 0~255，`A` 可省略（默认 255）。

| 函数 | 说明 |
|---|---|
| `mod.gui.set_style(t)` | 覆盖当前 Mod 窗口的配色，只影响调用者 |
| `mod.gui.rect(x,y,w,h,color)` | 绘制游戏原生背景块（沿用 `DrawInvBG` 纹理，不是无纹理纯色矩形） |
| `mod.gui.text_at(x,y,text,color,scale=1)` | 自定义位置、颜色、字号（`scale` 范围 0~4） |
| `mod.gui.button_at(x,y,w,h,label)` | 自定义位置和尺寸，松手命中时返回 `true` |
| `mod.gui.set_content_height(h)` | 声明内容高度，避免窗口高度不足 |

`set_style` 的键：`panel`、`title`、`button`、`hover`、`active`、`border`、`text`。

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

> 显式定位的控件需自己保持在窗口内容区内；当前没有裁剪 API、圆角参数等。

## 5. 图片与皮肤

资源由模组提供，PNG 放在模组 `Resources/lib/ui` 下，加载路径写 `lib/ui/example.png`。
加载仅注册资源；实际解码、上传和绘制在 Lua 回调结束后执行。可用于任务面板、商店、
背包卡片、状态条和小图标等，文字仍使用游戏字体。

| 接口 | 用法 |
|---|---|
| `load_image(path)` | 返回该模组独享的图片 ID，非法路径返回 `nil` |
| `image_status(id)` | 返回 `status,width,height`；status 为 `pending` / `ready` / `failed:原因` / `invalid` |
| `release_image(id)` | 将释放排到下一渲染帧，释放后的旧 ID 不可再用 |
| `image(id,x,y,w,h,color?,source?)` | 拉伸绘制；`source={sx,sy,sw,sh}` 为图片像素裁切，可用于图集 |
| `image_fit(id,x,y,w,h,mode?,color?)` | `contain` 完整等比显示；`cover` 等比填满并居中裁切 |
| `nine_slice(id,x,y,w,h,border,color?,edge?)` | 等宽四边九宫格；角固定，边和中央伸缩 |
| `image_button(id,x,y,w,h,label?,border?)` | 图片按钮，松手命中返回 `true`；`border>0` 使用九宫格 |
| `set_skin(panelId,border,iconId,iconSize=48,edge=0)` | 当前 Mod 窗口的面板皮肤与收起入口图片 |

- `color` 默认白色，可用透明度/颜色调制做高亮、淡出等效果。
- 九宫格的 `border` 是源图片像素，目标边框保持同样像素大小；窗口小于两倍边框时压缩边框。
  `edge` 可指定目标边框像素（例如源边框 300、显示 36）；不传时维持旧行为。
- `white.png`（1×1 白图）可用 `image` 拉伸加颜色形成真正的纯色矩形、进度条或装饰线；
  旧 `rect` 仍使用原版背景纹理。

```lua
local images={}
function on_gui()
    local g=mod.gui
    if not images.panel then
        images.panel=g.load_image("lib/ui/panel.png")
        images.icon=g.load_image("lib/ui/book.png")
        images.button=g.load_image("lib/ui/button.png")
        g.set_title("任务", "册")
        g.set_width(300)
        g.set_skin(images.panel,18,images.icon,48)
    end
    if g.image_button(images.button,12,20,250,44,"查看任务",12) then
        -- 修改你自己的页面状态
    end
    g.set_content_height(90)
end
```

`examples/TaskPanel` 提供金色、蓝色两套 SVG/PNG 皮肤，点击切换即可验证更换素材，无需改加载器。

### 约束与资源管理

- 仅支持 PNG，最大 2048×2048，文件不超过 16 MiB；最多缓存 128 张、合计 64 MiB RGBA 纹理。
- 路径限制在该模组私有资源目录，禁止绝对路径、父目录跳转和越界符号链接；不同模组的图片 ID 不能混用。
- 同一模组同一路径复用缓存；卸载模组后在渲染线程清理资源，隐藏窗口不等于卸载。
- 自定义控件需自己保持在窗口内容区内；暂无裁剪/遮罩、旋转、任意矢量曲线、模糊或着色器 API，
  不同形状和圆角可用透明 PNG 实现。
- 图片按钮当前使用矩形点击区域，高亮是颜色调制；点击反馈状态可由模组切换另一张图片实现，
  暂无按透明像素命中检测。

## 6. 虚拟键盘

`input_text` 不依赖系统输入法：点击输入框后弹出屏幕底部自绘键盘，并屏蔽该手指对游戏的
输入分发（键盘区域内的触摸不会落到游戏上）。键盘几何由 `includes/gui_keyboard_bounds.hpp`
统一定义，热区与实际按键一致。

## 7. 实现要点

- **绘制时机**：钩住 `Terraria.Main.Draw(GameTime)` 后置。此时游戏本帧的 `spriteBatch`
  已 `End`，加载器自行 `Begin`/`End` 绘制。
- **启动画面**：`Main.showSplash` 为真时跳过绘制。启动阶段贴图/字体尚未就绪，直接调用
  `DrawInvBG`/`DrawBorderString` 会解引用空资源而崩溃。
- **绘制指令队列**：`on_gui` 回调内的 `mod.gui.*` **只记录绘制指令**；回调返回、离开 Lua VM
  后，再由 C++ 统一执行托管绘制调用。原因见下节“Android 注意”。
- **输入**：指针位置与按键取自游戏自身（`Main.mouseX/Y`、`Main.mouseLeft` 等），因此各平台
  一致；指针悬停在窗口上时会屏蔽游戏的鼠标，避免误触。

## 8. 关闭 GUI（排查用）

设环境变量 `LUALOADER_GUI_DISABLE=1` 可整体禁用 GUI（不影响其它功能）。
日志中会输出 `[gui] native GUI disabled by LUALOADER_GUI_DISABLE`。

## 9. 向后兼容

`mod.gui.*` 的既有函数名、参数、返回语义与旧 ImGui 版本保持一致，旧 Mod 无需修改。
第 4、5 节的布局与图片接口为新增可选能力。
