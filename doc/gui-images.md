# 通用图片 UI（1.9.2-ui-test）

资源由模组提供，不与示例皮肤绑定。可用于任务面板、商店、背包卡片、状态条和小图标等不同界面。文字仍使用游戏字体。

在 `on_gui()` 内调用，所有 x/y 相对于窗口内容区，单位为屏幕像素。PNG 放在模组 Resources/lib/ui 中，加载路径写 `lib/ui/example.png`。加载仅注册资源；实际解码、上传和绘制在 Lua 回调结束后执行。

| 接口 | 用法 |
|---|---|
| `load_image(path)` | 返回该模组独享的图片 ID，非法路径返回 nil |
| `image_status(id)` | 返回 status,width,height；status 为 pending/ready/failed:原因/invalid |
| `image(id,x,y,w,h,color?,source?)` | 拉伸绘制；source={sx,sy,sw,sh} 为图片像素裁切，可用于图集 |
| `image_fit(id,x,y,w,h,mode?,color?)` | contain完整等比显示；cover等比填满并居中裁切 |
| `nine_slice(id,x,y,w,h,border,color?,edge?)` | 等宽四边九宫格；角固定，边和中央伸缩 |
| `image_button(id,x,y,w,h,label?,border?)` | 自定义图片按钮，松手命中返回true；border>0使用九宫格 |
| `set_skin(panelId,border,iconId,iconSize=48,edge=0)` | 当前模组窗口面板皮肤和收起入口图片 |
| `release_image(id)` | 将释放排到下一渲染帧，释放后的旧ID不可再使用 |

color={R,G,B,A}，通道0~255，默认白色。图片透明度和颜色调制可用来制作高亮、淡出等效果。九宫格的border是源图片像素，目标边框保持同样像素大小；窗口小于两倍边框时会压缩边框。

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

示例 `examples/TaskPanel` 提供金色、蓝色两套SVG/PNG皮肤。点击“切换蓝色皮肤/切换金色皮肤”验证更换素材；无需改加载器。Resources/lib/ui/white.png为1x1白图，可用`image`拉伸加颜色形成真正的纯色矩形、进度条或装饰线。旧`rect`仍使用原版背景纹理。

## 约束与资源管理

- 当前仅支持PNG，最大2048×2048，文件不超过16 MiB；最多缓存128张、合计64 MiB RGBA纹理。
- 路径限制在该模组私有资源目录，禁止绝对路径、父目录跳转和越界符号链接；不同模组图片ID不能混用。
- 同一模组同一路径复用缓存。卸载模组后在渲染线程清理资源；隐藏窗口不等于卸载。
- 自定义控件需自己保持在窗口内容区内，目前没有裁剪/遮罩、旋转、任意矢量曲线、模糊或着色器API。不同形状和圆角可通过透明PNG实现。
- 图片按钮当前使用矩形点击区域，高亮是颜色调制；点击反馈状态可由模组切换另一张图片实现。暂无按透明像素命中检测。
- Android 使用游戏XNA兼容层上传纹理，按正确的结构体ABI直调绘制，依赖IL2CPP GC句柄接口保留纹理；依赖缺失时记录 `[gui-image]` 错误并保留原版窗口入口。
- 顶层HUD覆盖和触摸拦截沿用1.8.0测试版，需要实机验证；本版本未宣称修复所有输入路径。

## 验证

已本地编译 Android ARM64/ARM32 和 Linux x64，完成资源路径、资源ID隔离、缓存复用与Lua页面流程检查。Windows/Linux x86的编译由附带GitHub Actions负责，未在此环境编译。PNG实际显示、透明边缘、裁切方向和触摸拦截尚未在游戏内验证。

1.9.2：收起图标可拖动，移动超过6个缩放像素即视为拖动，松手不展开；位置限制在屏幕内。edge可指定目标边框像素，例如源边框300、显示36。未传edge时维持旧行为。移除回退窗口使用DrawInvBG绘制的1px标题分隔线，避免出现粗黑条。

1.9.3修复：Android Color实际命名空间为Microsoft.Xna.Framework.Graphics，与桌面Microsoft.Xna.Framework不同。此前选错类型导致Draw overload匹配失败，纹理加载在PNG读取前被阻断。触摸Id字段改为缓存，避免每帧类型查询刷日志。图片加载成功、失败与绘制异常记录到模组private_dir/ui_image_runtime.log。

1.9.5文字测试修复：每个窗口先提交图片/背景批次，再在独立批次中绘制文字，避免延迟纹理分组把按钮文字覆盖；图片按钮标签按可用宽度自动缩放。六架构编译通过，仍需实际游戏截图确认缺字是否消除。
