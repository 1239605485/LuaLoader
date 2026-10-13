# LuaLoader 1.6.4 GUI 更新

采用石墨灰底色、青绿色强调色、圆角窗口与统一间距。普通按钮、滑块、折叠标题、进度条与虚拟键盘共享样式。`mod.gui.checkbox` 现在显示为开关，调用方式与布尔返回值保持兼容；输入框展示字段标签，编辑中的文本采用更清晰的背景。

各模组仍独立绘制窗口，支持拖动、缩放、折叠。新窗口默认错开放置，初始尺寸和缩放约束适应屏幕。虚拟键盘按可用宽度计算键宽，长文本预览在独立区域内裁剪。屏幕尺寸、缩放与输入在 NewFrame 之前更新。

## 参考

- [Material 3：配色与组件](https://developer.android.com/develop/ui/compose/designsystems/material3)
- [Material：深色主题](https://m2.material.io/develop/android/theming/dark)

参考视觉原则，在现有 Dear ImGui 内实现；没有添加 Android 原生 UI 框架。

## 构建与使用

这是完整源码包，不是可直接安装的 Android 发行包。GUI 仍需在编译时启用，避免原有非 GUI 构建被意外改变。

```bash
# Android 64 位；需要本机已配置 CMake、Ninja 和 Android NDK
./build.sh android-arm64 --gui

# Linux 64 位
./build.sh linux-x64 --gui
```

若需要安装包，继续使用原来的 `--package` 和 `--kernel-lib` 参数，详见 README 与 build.sh。桌面用 Insert 切换 GUI 输入模式；手机沿用已有触摸输入与屏幕虚拟键盘机制。只定义了 `mod.on_gui` 的模组会出现窗口。

## 验证

已进行 GUI 源码语法检查、完整 Linux x64 动态库编译和链接，以及直接使用生产样式与控件的 ImGui 离屏渲染检查。开关的标签点击、按下/释放、布尔值往返和跨帧保持通过；键盘横屏和竖屏边界通过。

预览使用示例模组内容，并非新增日志功能；预览环境没有中文系统字体，因此使用英文示例。实际界面沿用运行时中文字体加载。Android 二进制构建、Unity/GLES 叠加与手机触摸行为尚未实机验证。
