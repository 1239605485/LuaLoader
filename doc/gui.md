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

## 2. Lua 接口

控件（即时模式，返回值写回即可）：

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

窗口定位与显隐（1.7.0 新增）：

| 函数 | 说明 |
|---|---|
| `mod.gui.set_pos(x, y)` | 设置窗口左上角位置（屏幕像素） |
| `mod.gui.get_pos()` → `x, y` | 取当前窗口位置 |
| `mod.gui.set_width(w)` | 设置窗口宽度（高度随内容自适应） |
| `mod.gui.set_cursor(x, y)` | 移动布局光标（相对内容区左上角），之后控件从该处开始画 |
| `mod.gui.set_visible(v)` → `v` | 显示 / 隐藏窗口 |
| `mod.gui.is_visible()` → `bool` | 当前是否显示 |
| `mod.gui.show()` / `mod.gui.hide()` | 显示 / 隐藏 |

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

## 3. 实现要点

- **绘制时机**：钩住 `Terraria.Main.Draw(GameTime)` 后置。此时游戏本帧的
  `spriteBatch` 已 `End`，加载器自行 `Begin`/`End` 绘制。
- **启动画面**：`Main.showSplash` 为真时跳过绘制。启动阶段贴图/字体尚未就绪，
  直接调用 `DrawInvBG`/`DrawBorderString` 会解引用空资源而崩溃。
- **绘制指令队列**：`on_gui` 回调内的 `mod.gui.*` **只记录绘制指令**；回调返回、
  离开 Lua VM 后，再由 C++ 统一执行托管绘制调用。原因见下节“Android 注意”。
- **输入**：指针位置与按键取自游戏自身（`Main.mouseX/Y`、`Main.mouseLeft` 等），
  因此各平台一致；指针悬停在窗口上时会屏蔽游戏的鼠标，避免误触。
- **文本输入**：自绘输入框 + 屏幕底部虚拟键盘（不依赖系统输入法）。

## 4. 平台差异与已知问题

### 4.1 PE（Android）窗口位于游戏 HUD 之下

PE 上掩码来自游戏自身界面的绘制时序：加载器挂在 `Main.Draw` 后置，
而部分游戏 UI 在其之后才画，因此窗口会显示在 HUD/菜单**之下**。
这是已知的层级限制，暂未强行置顶（通过 `XNAUnityRunner` 等更靠后的点绘制会导致
只在切场景时闪现一帧，稳定性差）。

### 4.2 Android 按值结构体 FFI（重要）

Android 上，内核 `patchlib_method_invoke_args` 通过 libffi 按参数类型自动编组。
当方法含**按值传递的结构体参数/返回值**（如 `Vector2`、`Color`、`Rectangle`，
它们都带静态成员）时，内核生成的 `ffi_type` 布局有误，导致托管方法收到错位参数
并抛异常，进而在 C 层未捕获 → `std::terminate` → `SIGABRT`。

加载器因此在 Android 上对两个绘制方法改走 **原生函数指针 + 手写 ABI** 直调，
完全绕开上述 FFI 路径：

```cpp
using FnDrawInvBG  = void (*)(void* sb, Rectangle r, Color c);
using FnDrawString = Vector2 (*)(void* sb, void* text, Vector2 pos, Color color,
                                 float scale, float ax, float ay, int32_t maxChars);
```

> 详细的内核缺陷分析（三处：静态字段未过滤、值类型字段被当指针、返回类型句柄类型
> 不一致）见 TEFKernel 仓库 `documents/patchlib-ffi-struct-bug.md`。
> 内核修复后，加载器可撤掉该直调、统一切回 `patchlib_method_invoke_args`。

### 4.3 PC 光标 / tooltip 与窗口的层级

PC 的游戏光标与物品 tooltip 属于顶层接口图层。加载器**不再重绘**它们——重绘会在
非 100% UI 缩放（`UIScaleMatrix`）时产生第二个光标。当前实现仅绘制窗口本身，
因此当窗口恰好盖住光标时，光标会显示在窗口之下（已知小瑕疵）。

> 若后续需要“窗口在光标之下”，应改为把窗口绘制插入到接口图层顺序中（光标层之前），
> 而不是重绘光标。

## 5. 关闭 GUI（排查用）

设环境变量 `LUALOADER_GUI_DISABLE=1` 可整体禁用 GUI（不影响其它功能）。
日志中会输出 `[gui] native GUI disabled by LUALOADER_GUI_DISABLE`。

## 6. 向后兼容

`mod.gui.*` 的函数名、参数、返回语义与旧 ImGui 版本保持一致，旧 Mod 无需修改。
新增的定位/显隐函数为可选。
