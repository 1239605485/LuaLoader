# LuaLoader

LuaLoader 是 TEFKernel 的一个 ModLoader：内核负责注入和底层 hook，LuaLoader 负责把 Lua 脚本跑起来，
并把内核的 patchlib 能力通过一张 `mod` 表交给脚本使用。

一句话：**Mod 用 Lua 写，不用编译，一份脚本全平台通用。**

> 作者：lzup333

📖 **API 文档**：[`doc/api.md`](doc/api.md) —— 极简教学，从最小 Mod 到完整 `mod.patch` 用法。

## 它做什么

- 加载 Mod 目录里的 `main.lua`，跑脚本、管生命周期、装钩子；
- 给脚本提供 `mod` 表：日志、私有目录读写、`mod.patch.*`（类型/字段/方法/钩子）；
- 每个 Mod 用独立的 Lua 状态机，互不干扰；
- 目前支持 **Android arm64 / arm**、**Linux x64 / x86** 和 **Windows x64 / x86**。

## 目录结构

```
LuaLoader/
├── CMakeLists.txt / CMakePresets.json   # 构建脚本和预设
├── Info.json / Manifest.json            # 加载器包信息
├── includes/                            # 头文件（core / logger / lua_engine / lua_api / tefkernel）
├── mod-api/                             # 日志级别定义
├── lib/                                 # 内置 Lua 5.4 和 spdlog
└── src/                                 # 源码
```

## 编译

需要 CMake ≥ 3.28 和支持 C++17 的编译器。Lua 和 spdlog 已经放在 `lib/` 里，不用另外装。

```bash
cmake --preset linux-x86_64-release
cmake --build --preset linux-x86_64-release
```

产物是 `libloader.<平台>.<架构>.so`（Windows 为 `.dll`；Linux x64 就是 `libloader.linux.x64.so`）。

Android 需要 NDK：

```bash
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.0.13004108
cmake --preset android-arm64-release -DANDROID_NDK="$ANDROID_NDK_HOME"
cmake --build --preset android-arm64-release
# arm32 用 android-arm32-release
```

## 构建

LuaLoader随仓提供一个build.sh,用于构建或编译,构建需一个libtefkernel

构建产物包：
```bash
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.0.13004108/
export TEFKERNEL_LIB=./libtefkernel.linux.x86_64.so 
./build.sh -r --package
```

## 写一个 Mod

一个 Mod 就是一个目录，结构和 KernelLoader 的 Mod 一样，只是入口从 `.so` 换成了 `main.lua`：

```
my_lua_mod/
├── Info.json          # Mod 元信息
├── Manifest.json      # 清单，parentLoader 写 lzup333.lualoader
├── luamod.json        # 告诉加载器入口脚本是谁
└── Resources/
    └── lib/
        └── main.lua   # 入口脚本
```

`luamod.json`：

```json
{ "main": "main.lua" }
```

`main.lua` 长这样（示例：每帧把玩家魔力补满）：

```lua
mod.meta = { pkg_id = "com.example.mymod", version = "1.0.0" }

local stat_mana
local stat_mana_max

function lock_mana(instance)
    local max = mod.patch.get_field_value(stat_mana_max, instance, "int32")
    mod.patch.set_field_value(stat_mana, instance, max, "int32")
end

function setup()
    local player = mod.patch.get_type("Terraria", "Player")
    if not player then return end

    stat_mana = mod.patch.get_field(player, "statMana")
    stat_mana_max = mod.patch.get_field(player, "statManaMax")
    mod.patch.install_hook(mod.patch.get_method(player, "ResetEffects", 0), { postfix = lock_mana })
end

todo_list = { "setup" }
```

加载器只跑入口脚本（默认 `main.lua`），其它 `.lua` 文件不会自动执行，需要的话用 `require("文件名")` 按需加载。

## `mod` 表

环境信息：

- `mod.id`：Mod 的 id
- `mod.platform` / `mod.arch`：当前平台和架构（`android`/`linux`…、`arm64`/`arm`/`x64`/`x86`）
- `mod.private_dir` / `mod.mod_dir` / `mod.version`

日志：

- `mod.log(level, ...)`，level 是 `"trace"`/`"debug"`/`"info"`/`"warn"`/`"warning"`/`"error"`/`"critical"`/`"fatal"`（或数字 0~6）
- 快捷方法：`mod.trace` / `mod.debug` / `mod.info` / `mod.warn` / `mod.error` / `mod.fatal`

私有目录读写（只能在 `mod.private_dir` 里，禁止 `..` 和绝对路径）：

- `mod.read_file(name)` → 内容字符串，失败返回 `nil, err`
- `mod.write_file(name, data)` → 成功返回 `true`
- `mod.file_exists(name)` → `true`/`false`

生命周期：

- `mod.init()`：Mod 初始化时调用
- `mod.cleanup()`：Mod 卸载时调用

## 内置 GUI（`mod.gui` / `mod.on_gui`）

LuaLoader 内置原生 GUI：Mod 直接用 Lua 画界面，无需额外的原生插件。

- 只要定义了 `function mod.on_gui()`，加载器就会**自动为该 Mod 创建一个独立窗口**并每帧回调，
  Mod 不用自己管窗口的绘制/拖动/输入。
- 界面**直接使用游戏自带的 XNA / Unity SpriteBatch 绘制**（PC：XNA/FNA；PE：Unity+XNA 兼容层），
  因此不需要额外的渲染后端，也不依赖 OpenGL 叠加或 `eglSwapBuffers`。
- 任何定义了 `on_gui` 的 Mod 都会被绘制，无需调试模式。
- 指针悬停在窗口上时，加载器会屏蔽游戏鼠标，避免误触游戏操作。
- 高 DPI 自动缩放：字号与控件随屏幕尺寸一起放大，手机上看不费眼。

```lua
local speed = 3.0
local name = ""

function mod.on_gui()
    mod.gui.text("示例界面")
    mod.gui.separator()
    speed = mod.gui.slider("speed", speed, 1.0, 10.0)
    if mod.gui.button("点我") then mod.info("clicked") end
    name = mod.gui.input_text("名字", name)   -- 自带屏幕虚拟键盘（手机友好）
    mod.gui.text("你好，" .. name)
end
```

可用控件：`text` / `text_wrapped` / `button` / `checkbox` / `slider` / `same_line` /
`separator` / `spacing` / `collapsing_header` / `progress_bar` / `input_text`。

窗口定位与显隐：`set_pos` / `get_pos` / `set_width` / `set_cursor` / `set_visible` /
`is_visible` / `show` / `hide`。

```lua
-- 指定绘制位置 + 控制出现/消失
mod.gui.set_pos(20, 20)
mod.gui.set_visible(visible)   -- 隐藏时仍会回调 on_gui，可在此重新 show()
```

> `input_text` 自绘输入框 + 底部虚拟键盘，点虚拟键不会失焦。

> **向后兼容**：1.7.0 起 GUI 由 Dear ImGui 改为原生 XNA/Unity 绘制，但 `mod.gui.*` 的
> 函数名、参数与返回语义保持不变，旧版 GUI Mod 无需修改即可继续使用。

> 设计说明、平台差异与已知限制见 [`doc/gui.md`](doc/gui.md)。

## 平台差异

推荐用“任务清单”：把函数定义写在外面，清单里只写函数名，加载器按 `todo_list` →
`todo_list_<平台>` → `todo_list_<平台>_<架构>` 的顺序执行，只执行与当前平台匹配的清单。

```lua
function setup()         mod.info("通用逻辑") end
function android_setup() mod.info("移动端逻辑") end

todo_list = { "setup" }
todo_list_android = { "android_setup" }        -- 只在 Android 执行
-- todo_list_android_arm64 = { "..." }         -- 只在 Android arm64 执行
```

清单里的元素也可以直接写函数（`{ setup }`）。零星差异也可以用 `if mod.platform == "android" then ... end`。

## `mod.patch`（调用内核能力）

> 完整 API 与教学示例见 [`doc/api.md`](doc/api.md)。

取类型/字段/方法：

- `mod.patch.get_type(ns, name)`：按命名空间+类名取类型，如 `get_type("Terraria", "Player")`
- `mod.patch.get_field(type, name)` / `mod.patch.get_property(type, name)`
- `mod.patch.get_method(type, name[, argc])`
- `mod.patch.get_method_by_names(type, name, {参数名...})`：按参数名精确选重载，避免选错
- `mod.patch.property_get_method(prop)` / `mod.patch.property_set_method(prop)`：取属性访问器
- `mod.patch.new_instance(type)` / `mod.patch.get_parent(type)` / `mod.patch.type_name(type)`
- `mod.patch.get_inner_type(parent, name)`：取嵌套类型，如 `get_inner_type(ItemID, "Sets")`
  （`ItemID.Sets` / `PrefixLegacy.ItemSets` 这类嵌套类只能用这种方式取）
- `mod.patch.get_basic_type(name)`：`"int32"`/`"float"`/`"bool"`/`"object"` 等
- `mod.patch.free(handle)`

内省 / 枚举 / 选重载 / 泛型（1.5.0 新增，详见 `doc/api.md`）：

- `get_full_name(type)` / `get_namespace(type)`
- `get_fields(type)` / `get_methods(type)` / `get_properties(type)` / `get_inner_types(type)`
- `get_method_by_param_types(type, name, {类型句柄...})` / `get_method_by_signature(type, name, {类型句柄...}, {参数名...})`
  （表里放类型**句柄**，如 `get_basic_type("int32")`；后者参数名表可省略）
- `make_generic_type(def, {type...})` / `make_generic_instance(method, {type...})`
- `method_name/param_count/token/is_instance/is_static`、`field_name/is_const/is_instance/is_static`、`property_name`
- 容器：`dictionary_create/add/set_value/get_value/length/remove/clear`、`list_create/add/remove/remove_at/clear/copy_from/get_array`、`array_empty`
  （`*_create` 传类型**句柄**；`add/set_value/get_value/remove` 的类型参数是类型**名**字符串，细节见 `doc/api.md`）
- 按值结构体参数（仅 Android）：`struct_arg({"float","float"},{x,y})` + `invoke_value_args(method[, instance], {arg...})`

读写字段：

```lua
mod.patch.get_field_value(field, instance[, type])
mod.patch.set_field_value(field, instance, value[, type])
```

`type` 可以省略（由内核查），但建议显式写，例如 `"int32"`。**Android 上请一定写**，因为
Android 走的是字段真实指针，类型不明确容易读错。

托管字符串（`System.String`）可以直接当 Lua 字符串读写，把类型写成 `"string"` 即可：

```lua
local name = mod.patch.get_field_value(field, instance, "string")  -- 返回 Lua 字符串
mod.patch.set_field_value(field, instance, "你好", "string")
local s = mod.patch.array_at(array, i, "string")                   -- 数组元素也可以
```

需要手动构造/读取时：

```lua
local h = mod.patch.string_create("hello")  -- 托管字符串句柄（由 Lua GC 管理）
mod.patch.string_value(h)                   -- "hello"（也可直接传普通 Lua 字符串）
mod.patch.string_empty(h)                   -- false
mod.patch.string_length(h)                  -- 5
```

创建对象实例：

```lua
-- 无参：与旧版一致
local obj = mod.patch.new_instance(type)
-- 带参：自动按 ".ctor" 参数个数匹配构造函数
local obj2 = mod.patch.new_instance(type, 1, "x")
-- 也可以显式拿构造函数再构造
local ctor = mod.patch.get_method(type, ".ctor", 2)
local obj3 = mod.patch.construct(ctor, 1, "x")
```

句柄的生命周期：

- 普通 API 返回的对象句柄是**借用**的，只在当前调用内有效（hook 里的 `instance`/对象参数
  在回调结束后会被内核释放）。**不要**把 hook 里的 `instance` 直接存起来跨帧使用。
- 需要跨帧缓存时，用 `mod.patch.retain(handle)` 拿到**由 Lua GC 托管**的句柄副本，
  不必手动 `free`：

  ```lua
  local npc = mod.patch.retain(instance)   -- 可以安全地存进表里
  ```

- 或者给 hook 加一个 `copy = true`，让 `instance`、对象参数、返回值自动变成托管副本：

  ```lua
  mod.patch.install_hook(method, { postfix = on_ai, copy = true })
  ```

  托管副本只是同一对象的另一个句柄，读写字段仍然作用于原对象；Lua GC 会在不再引用时
  自动释放。默认不开 `copy`，行为与旧版完全一致。

结构体字段（如 `Vector2`）与数组：

```lua
-- Vector2 等以两个 float 开头的 8 字节结构体
local x, y = mod.patch.get_field_vec2(field, instance)
mod.patch.set_field_vec2(field, instance, x, y)

-- 原始字节读写（配合 string.pack/unpack 可处理任意结构体）
local raw = mod.patch.get_field_raw(field, instance[, size])  -- 省略 size 时按字段类型推断；失败返回 nil
mod.patch.set_field_raw(field, instance, raw)                 -- raw 长度需为 1~16 字节

-- 数组（如 Main.npc / Main.projectile 这类对象数组）
local n = mod.patch.array_length(array)
local obj = mod.patch.array_at(array, i)               -- 省略类型时按对象(指针)处理
local num = mod.patch.array_at(array, i, "float")      -- 值类型数组需给类型名

-- 写数组 / 新建数组
mod.patch.array_set(array, i, value[, type])           -- 写元素
mod.patch.array_fill(array, value[, type])             -- 填充
mod.patch.array_create(size, elem_type)                -- 新建托管数组
local raw = mod.patch.array_at_raw(array, i, size)     -- 原始字节读
mod.patch.array_set_raw(array, i, raw)                 -- 原始字节写
```

对象数组的句柄可通过静态字段取得，例如
`mod.patch.get_field_value(mod.patch.get_field(main, "projectile"), nil, "object")`。

C 快通道（内存 / 指针，Android）：把“逐元素跨边界”压成“批量一次读”，可用于**任何平坦内存布局**，
不只是世界 Tile——原生指针数组、内联结构体数组、成批对象指针、纹理缓冲等都能用。

```lua
local ptr = mod.patch.field_pointer(field, instance)   -- 仅 Android 返回真实指针，桌面端 nil
local raw = mod.patch.mem_read(ptr, offset, size)      -- 原始字节读写
mod.patch.mem_write(ptr, offset, raw)
mod.patch.ptr_add(ptr, byte_offset)                    -- 指针偏移
mod.patch.ptr_deref(ptr, byte_offset)                  -- 读取指针值（指针链）
mod.patch.mem_read_values(ptr, byte_offset, count, type)   -- 批量读 -> 表
mod.patch.mem_write_values(ptr, byte_offset, table, type)  -- 批量写
mod.patch.field_size(field)                            -- 字段字节大小（不可用返回 0）
```

> 桌面端 `field_pointer` 返回 `nil`，Mod 自动退回托管路径（`array_at` / `Framing.GetTileSafely` 等），
> 从而保持**一份脚本全平台**。具体用法与示例见 [`doc/api.md`](doc/api.md)。

调用方法（实例方法第 2 参传 `instance`，静态方法不传）：

```lua
mod.patch.invoke(method, [instance, ] ...)
```

引用类型返回一个**借用句柄**（仅当前调用内有效，要跨帧缓存请 `retain`），`void` 返回 `nil`，
调用失败返回 `false`。

钩子 `postfix` **默认忽略返回值**（与旧版一致，保证兼容）。若需要它覆盖原方法返回值，
安装时加 `override_result = true`（或 `result = true`）：

```lua
mod.patch.install_hook(method, {
    override_result = true,
    postfix = function(instance, args, result)
        return 999   -- 覆盖返回值（类型需与签名匹配；返回 nil 表示不修改）
    end,
})
```

装钩子：

```lua
local hook = mod.patch.install_hook(method, {
    -- 原方法执行前：返回 true 表示跳过原方法（可再返回一个值作为返回值）
    prefix = function(instance, args, result) return false end,
    -- 原方法执行后：result 是原方法的返回值
    postfix = function(instance, args, result) end,
})
hook:remove()   -- 手动卸载
```

- `prefix` / `postfix` 至少写一个；
- 最多 **1024** 个钩子（全加载器共享，`remove()`/卸载后释放可复用），钩子装上后一直有效，直到 `hook:remove()` 或 Mod 卸载（丢弃返回值不会导致失效）。


## 调试模式与热重载

LuaLoader 有一个隐藏配置文件，放在**加载器自己的私有目录**（普通玩家在 Mod 包里看不到）：

```
<工作目录>/modloader/private/lzup333.lualoader/config.json
```

```json
{ "debug": false }
```

把 `debug` 改成 `true` 并重启游戏后，就能在**游戏聊天框**输入：

- `/reload` —— 重新扫描并同步所有 Mod：
  - 新启用的 Mod → **热加载**；
  - 已禁用的 Mod → 热卸载；
  - 正在运行的 Mod → 热重载（重跑脚本与 `setup`）。

改完 `.lua` 保存、再 `/reload` 即可生效，**不用重新编译、不用退游戏**，热重载后会自动重新安装钩子。

> 提示：管理器的“启用/禁用”是异步落盘的，切换后等几秒再 `/reload`。

## 打包和部署

打包需要 TEFPkg-Tool：把各平台的 `libloader.<平台>.<架构>.so` 打进 `lualoader.tefpkg`，
再和 `Info.json`、`Manifest.json` 一起压缩成一个 zip 就是加载器安装包。

部署后内核会把 Mod 的 `Resources/` 释放到 Mod 私有目录，所以入口脚本最终在
`<private_dir>/lib/main.lua`；加载器会按下面顺序找入口脚本：

1. `<private_dir>/lib/<main>`
2. `<private_dir>/<main>`
3. `<配置文件目录>/<main>`
4. `<配置文件目录>/Resources/lib/<main>`

## 原生模块（C/C++ 扩展）

LuaLoader **支持加载 C/C++ 原生模块**（`.so` / `.dll`），用于性能敏感或 Lua 不好实现的场景。

**目录约定**（随 Mod 包一起分发，按平台放置）：

```
Resources/native/<平台>_<架构>/<模块名>.so     # 如 linux_x64 / android_arm64 / windows_x64
Resources/native/<平台>/<模块名>.so            # 次选（不区分架构）
```

部署后对应 `<私有目录>/native/<平台>_<架构>/` 与 `<私有目录>/lib/native/...`，加载器会自动把它加入
`package.cpath` 并以 `RTLD_GLOBAL` 预加载。

**在 Lua 里使用**：就是普通 C 模块，`require` 即可：

```lua
local mymod = require("mymod")   -- 加载 <模块名>.so，调用 luaopen_mymod
```

**在 C 侧怎么取 API**：推荐用 loader 注入的 API 表（全平台一致）：

```c
#include "lualoader_mod.h"

static const ll_api_t *LL = NULL;
LL_EXPORT void ll_set_api(const ll_api_t *api) { LL = api; }  /* loader 注入 */

int luaopen_mymod(lua_State *L) {
    if (!LL) return 0;                                        /* 旧 loader 未注入则优雅失败 */
    LL_CACHE(p_pushinteger, lua_pushinteger, void, (lua_State *, long long));
    p_pushinteger(L, 1);   /* 注意：像 lua_pushcfunction / lua_tointeger 是宏，没有同名符号 */
    return 1;
}
```

- `LL->lookup("名字")` 可解析 loader 自身导出的符号：`lua_*`、`luaL_*` 函数，以及 `patchlib_*`
  函数指针变量（`lookup` 会自动解引用，直接返回可调用的函数指针）
- `lua.h` 里的宏（如 `lua_pushcfunction`、`lua_tointeger`、`lua_tostring`、`lua_newtable`、
  `lua_pop`、`luaL_newlib`）没有导出符号，请改用底层真实函数（`lua_pushcclosure`、`lua_tointegerx`、
  `lua_tolstring`、`lua_createtable`、`lua_settop`、`luaL_setfuncs`）
- Android 上模块库会先复制到应用私有目录再加载（绕开 linker namespace 限制）
- 只有在 Linux/桌面，才可以省掉 API 表、直接 `extern` 引用 `lua_*` / `patchlib_*`

> 完整指南（目录约定 / 编译 / Windows import lib / 排查）见 [`doc/native.md`](doc/native.md)。



## Lua 能力与安全提示

- LuaLoader **打开 Lua 5.4 的全部标准库**：`base`、`table`、`string`、`math`、`utf8`、
  `coroutine`、`io`、`os`、`debug`、`package`，Mod 可以使用完整 Lua 代码
  （`io.open` / `os.*` / `debug.*` / `dofile` / `loadfile` / `load` 等均可）。
- `package.path` 会包含 Mod 目录，`require` 可加载 Mod 自带的 Lua 模块。
- **原生 C 模块已开放**：Mod 可自带 `native/<平台>_<架构>/*.so`（加载器会加入 `package.cpath` 并预加载），
  详见上文「原生模块（C/C++ 扩展）」。这就是说，Mod 能执行原生代码，请只安装信任来源的 Mod。

> ⚠️ **安全提示**：这意味着 Mod 脚本可以读写文件、执行系统命令、加载原生模块。
> 安装前建议先**手动打开 Mod 压缩包查看其中的 `.lua` 脚本与 `native/` 目录**（也可以丢给 AI 帮你分析
> 它做了什么），确认无误再启用。

## 许可证

LuaLoader 本体是 **AGPL-3.0-or-later**（见 [`LICENSE`](LICENSE)）。

随仓库分发的第三方组件、版本与许可证见 **[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)**；
许可证**原文**在 `LICENSES/`（来源见该说明文件）：

- Lua 5.4.8、spdlog 1.17.0（及其内置的 {fmt}）、nlohmann/json 3.12.0 —— 均为 **MIT**；
- `includes/tefkernel/` 来自 **TEFKernel**：仓库整体 **AGPL-3.0**，随附头文件按文件头为 **MIT**
  （例外：`tefpackage/tefpkg.h` 标注 AGPL-3.0）；
- `mod-api/mod_logger.h` 来自 **KernelLoader**（作者同为 eternalfuture-e38299），**MIT**。
