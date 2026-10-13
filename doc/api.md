# LuaLoader API 文档

LuaLoader 用 Lua 写 Mod，不用编译，一份脚本全平台通用。每个 Mod 跑在**独立**的 Lua 状态机里，
脚本通过全局表 `mod` 访问一切能力。

> 图省事的话，先看 [最小 Mod](#最小-mod)，再按需查下面的表。

## 目录

- [最小 Mod](#最小-mod)
- [生命周期](#生命周期)
- [`mod` 表](#mod-表)
  - [环境信息](#环境信息)
  - [日志](#日志)
  - [文件](#文件)
- [`mod.gui`：原生图形界面](#modgui原生图形界面)
- [`mod.patch`：调用内核](#modpatch调用内核)
  - [取类型与成员](#取类型与成员)
  - [字段读写](#字段读写)
  - [数组](#数组)
  - [属性桥接](#属性桥接)
  - [C 快通道（内存 / 指针，Android）](#c-快通道内存--指针android)
  - [托管字符串](#托管字符串)
  - [创建实例](#创建实例)
  - [调用方法](#调用方法)
  - [Hook](#hook)
  - [句柄与生命周期](#句柄与生命周期)
- [调试与热重载](#调试与热重载)
- [常见陷阱](#常见陷阱)
- [完整示例](#完整示例)

---

## 最小 Mod

一个 Mod 就是一个目录：

```
my_mod/
├── Info.json               # 展示信息
├── Manifest.json           # parentLoader 写 "lzup333.lualoader"
├── luamod.json             # 入口脚本
└── Resources/
    └── lib/
        └── main.lua
```

`luamod.json`：

```json
{ "main": "main.lua" }
```

`main.lua`：

```lua
mod.meta = { pkg_id = "lzup.lua.hello", version = "1.0.0" }

function setup()
    mod.info("hello from " .. mod.id)
end

todo_list = { "setup" }
```

加载器只执行入口脚本；其它 `.lua` 用 `require("文件名")` 按需加载（`package.path` 已包含 Mod 目录）。

---

## 生命周期

**任务清单**是最推荐的写法：把函数定义在外面，清单里只写函数名，加载器按顺序执行：

```lua
function setup() end
function android_setup() end

todo_list = { "setup" }                    -- 通用
todo_list_android = { "android_setup" }    -- 仅 Android
-- todo_list_android_arm64 = { ... }       -- 仅 Android arm64
```

- 执行顺序：`todo_list` → `todo_list_<平台>` → `todo_list_<平台>_<架构>`。
- 清单元素可以是函数名（字符串）或函数本身（`{ setup }`）。
- 也可以用 `mod.init()` / `mod.cleanup()`：初始化时在任务清单之后调用，卸载时调用 `mod.cleanup()`。
- 零星差异可直接判断：`if mod.platform == "android" then ... end`。
- 定义全局函数 `on_gui`（`mod.on_gui`）后，加载器会为这个 Mod 开一个窗口并每帧回调它，见 [`mod.gui`](#modgui原生图形界面)。

---

## `mod` 表

### 环境信息

| 字段 | 说明 |
|---|---|
| `mod.id` | Mod 的唯一 id（内核分配） |
| `mod.version` | 版本字符串 |
| `mod.platform` | `android` / `linux` / `windows` / `macos` / `ios` / `unknown` |
| `mod.arch` | `arm64` / `arm` / `x64` / `x86` / `unknown` |
| `mod.mod_dir` | 入口脚本所在目录（部署后通常是 `<private_dir>/lib`），`require` 的搜索根 |
| `mod.private_dir` | Mod 私有数据目录；文件 API 的根，也是内核释放 `Resources/` 的位置 |

`mod.meta`（可选）可覆盖 `luamod.json` 里的信息：

```lua
mod.meta = { pkg_id = "...", version = "1.0.0", version_code = 1, api_version = 1 }
```

### 日志

```lua
mod.log("info", "x =", 42)          -- 通用写法；level 为字符串（trace/debug/info/warn/warning/error/critical/fatal）或数字 0~6
mod.info("玩家 " .. name .. " 已锁定")   -- 快捷方法
```

快捷方法：`mod.trace` / `mod.debug` / `mod.info` / `mod.warn` / `mod.error` / `mod.fatal`。
多参数会用空格拼接，非字符串会转成字符串。

### 文件

只能在 `mod.private_dir` 内读写，禁止绝对路径和 `..`：

```lua
local data, err = mod.read_file("config.txt")  -- 失败返回 nil, err
mod.write_file("config.txt", "hello")          -- 成功返回 true
mod.file_exists("config.txt")                  -- true / false
```

> Mod 可使用完整的 Lua 标准库（`io` / `os` / `debug` 等），并可自带 C/C++ 原生模块
> （`Resources/native/<平台>_<架构>/`，详见 [`native.md`](native.md)）。

---

## `mod.gui`：原生图形界面

定义了 `on_gui` 的 Mod 会得到一个可拖动的窗口，加载器**每帧**调用一次该函数。
界面直接使用游戏自带的 XNA / Unity SpriteBatch 绘制，因此 PC（XNA/FNA）与 PE（Unity）表现一致，
且无需额外渲染后端。所有控件都是即时模式：返回值即当前状态，直接写回即可。

```lua
local speed = 3.0
local enabled = true
local name = ""

function mod.on_gui()
    mod.gui.text("我的 Mod")
    mod.gui.separator()
    speed   = mod.gui.slider("速度", speed, 1.0, 10.0)
    enabled = mod.gui.checkbox("启用", enabled)
    name    = mod.gui.input_text("名字", name)
    if mod.gui.button("点我") then
        mod.info("点击，名字 = " .. name)
    end
    mod.gui.same_line()
    mod.gui.text("速度 = " .. speed)
end
```

| 函数 | 返回值 | 说明 |
|---|---|---|
| `mod.gui.text(s)` | – | 绘制一行文本 |
| `mod.gui.text_wrapped(s)` | – | 按窗口宽度自动换行的文本 |
| `mod.gui.button(label)` | `bool` | 按钮；本帧被点击返回 `true` |
| `mod.gui.checkbox(label, v)` | `bool` | 复选框，返回切换后的值 |
| `mod.gui.slider(label, v, min, max)` | `number` | 拖动滑块，返回新值 |
| `mod.gui.input_text(label, v)` | `string` | 文本输入框；点击后在屏幕底部弹出虚拟键盘 |
| `mod.gui.same_line()` | – | 下一个控件与上一个同行 |
| `mod.gui.separator()` | – | 水平分隔线 |
| `mod.gui.spacing()` | – | 空行 |
| `mod.gui.collapsing_header(label)` | `bool` | 可折叠标题，返回展开状态 |
| `mod.gui.progress_bar(v)` | – | 进度条（0.0 ~ 1.0） |
| `mod.gui.set_pos(x, y)` | – | 设置窗口左上角位置（屏幕像素） |
| `mod.gui.get_pos()` | `x, y` | 取当前窗口位置 |
| `mod.gui.set_width(w)` | – | 设置窗口宽度（高度随内容自适应） |
| `mod.gui.set_cursor(x, y)` | – | 移动布局光标（相对内容区左上角），之后控件从该处开始绘制 |
| `mod.gui.set_visible(v)` | `bool` | 显示/隐藏窗口，返回设定值 |
| `mod.gui.is_visible()` | `bool` | 当前是否显示 |
| `mod.gui.show()` / `mod.gui.hide()` | – | 显示 / 隐藏窗口 |

> 窗口位置与显隐由 Mod 控制。隐藏时**仍会回调 `on_gui`**（方便 Mod 在回调里通过
> `mod.gui.show()` / `set_visible(true)` 重新显示），但控件不绘制、不响应输入。
> 例如用一个热键/聊天指令改变自己的标志位，在 `on_gui` 里 `mod.gui.set_visible(flag)`。

> 窗口背景、标题栏、拖动与输入屏蔽都由加载器处理；Mod 只需关心控件本身。
> 指针悬停在窗口上时，加载器会屏蔽游戏鼠标，避免误触游戏操作。
>
> 1.7.0 起 GUI 由 Dear ImGui 改为原生 XNA/Unity 绘制，但 `mod.gui.*` 的函数名、参数与
> 返回语义保持不变，旧版 GUI Mod 无需修改即可继续使用。

---

## `mod.patch`：调用内核

`mod.patch` 把内核 patchlib 的类型 / 字段 / 方法 / 钩子能力交给 Lua。所有“句柄”在 Lua 里是
lightuserdata（借用）或 GC 托管 userdata（自有），都能直接互相传参。

### 取类型与成员

```lua
local player = mod.patch.get_type("Terraria", "Player")  -- 命名空间 + 类名
local field  = mod.patch.get_field(player, "statMana")
local prop   = mod.patch.get_property(player, "someProp")
local method = mod.patch.get_method(player, "ResetEffects", 0)  -- 无参重载
local method2 = mod.patch.get_method(player, "Foo")             -- 只有一个重载时可省略个数
```

其它：

| 函数 | 说明 |
|---|---|
| `get_basic_type(name)` | 基础类型，如 `"int32"` / `"float"` / `"object"` |
| `get_method_by_names(type, name, {names...})` | 按参数名精确选重载（避免选错同名同参个数的方法） |
| `property_get_method(prop)` / `property_set_method(prop)` | 从属性句柄取 getter / setter 方法句柄 |
| `type_name(handle)` | 类型名 |
| `get_parent(type)` | 父类型 |
| `get_inner_type(parent, name)` | 嵌套类型（如 `ItemID.Sets` / `PrefixLegacy.ItemSets`，见下） |
| `get_full_name(type)` | 完整名（命名空间 + 类名） |
| `get_namespace(type)` | 命名空间 |
| `get_fields(type[, 含父类])` | 字段句柄表 `{ field, ... }` |
| `get_methods(type[, 含父类])` | 方法句柄表 `{ method, ... }` |
| `get_properties(type[, 含父类])` | 属性句柄表 `{ property, ... }` |
| `get_inner_types(type[, 含父类])` | 嵌套类型句柄表 |
| `get_method_by_param_types(type, name, {类型句柄...})` | 按参数类型精确选重载（表里放类型**句柄**，如 `get_basic_type("int32")`） |
| `get_method_by_signature(type, name, {类型句柄...}, {参数名...})` | 按参数类型（句柄）+ 参数名选重载；第二个表可省略 |
| `make_generic_type(generic_def, {type...})` | 实例化泛型类型（如 `Dictionary<,>`） |
| `make_generic_instance(method, {type...})` | 实例化泛型方法 |
| `method_name(m)` / `method_param_count(m)` / `method_token(m)` | 方法名 / 参数个数 / Token（可当缓存键） |
| `method_is_instance(m)` / `method_is_static(m)` | 是否实例 / 静态方法 |
| `field_name(f)` / `field_is_const(f)` / `field_is_instance(f)` / `field_is_static(f)` | 字段名 / 只读 / 实例 / 静态 |
| `property_name(p)` | 属性名 |
| `array_empty(arr)` | 数组是否为空 |
| `dictionary_create(key_type, value_type[, cap])` | 创建 `Dictionary<,>`；两参是类型**句柄** |
| `dictionary_add(dict, k, kt, v, vt)` | 新增键值对（`kt`/`vt` 是类型**名**，见下注） |
| `dictionary_set_value(dict, k, kt, v, vt)` | 修改键值对（`kt`/`vt` 是类型**名**） |
| `dictionary_get_value(dict, k, kt, vt)` | 按键取值（`kt`/`vt` 是类型**名**），取不到返回 `nil` |
| `dictionary_length(dict)` / `dictionary_remove(dict, k, kt)` / `dictionary_clear(dict)` | 长度 / 删除（`kt` 是类型**名**）/ 清空 |
| `list_create(elem_type[, cap])` | 创建 `List<>`；参数是类型**句柄** |
| `list_add(list, v, vt)` / `list_remove(list, v, vt)` | 增删元素（`vt` 是类型**名**） |
| `list_remove_at(list, i)` / `list_clear(list)` / `list_copy_from(list, array)` / `list_get_array(list)` | 按下标删除 / 清空 / 从数组填充 / 取内部数组 |
| `struct_arg({"float","float"}, {x, y})` | 打包按值结构体参数（如 `Vector2`，**仅 Android**） |
| `invoke_value_args(method[, instance], {arg...})` | 调用含按值结构体参数的方法（表项可用 `struct_arg`，**仅 Android**） |
| `is_valid(handle)` | 句柄是否有效 |
| `free(handle)` | 手动释放句柄（一般不用，见下） |

> **容器（Dictionary / List）的类型参数怎么给？**
> - `dictionary_create` / `list_create` 需要类型**句柄**（用 `get_type(...)` 或
>   `get_basic_type("int32")` 得到），用来实例化 `Dictionary<TKey,TValue>` / `List<T>`。
> - `dictionary_add/set_value/get_value/remove`、`list_add/remove` 的 `kt` / `vt` 是类型**名字符串**
>   （`"int32"` / `"float"` / `"bool"` / `"object"` …），内核据此把 Lua 值编组进容器。**不要传句柄。**
>
> ```lua
> local int_t = mod.patch.get_basic_type("int32")          -- 类型句柄
> local dict  = mod.patch.dictionary_create(int_t, int_t)
> mod.patch.dictionary_add(dict, 1, "int32", 100, "int32") -- 类型名用字符串
> local v = mod.patch.dictionary_get_value(dict, 1, "int32", "int32")  -- 100
> ```

> **嵌套类型**：像 `Terraria.ID.ItemID.Sets`、`Terraria.GameContent.Prefixes.PrefixLegacy.ItemSets`
> 这类嵌套类，用 `get_type("Terraria.ID", "ItemID.Sets")` 在 Android 上取不到。
> 要先取外层类型，再用 `get_inner_type`：
>
> ```lua
> local item_id = mod.patch.get_type("Terraria.ID", "ItemID")
> local sets    = mod.patch.get_inner_type(item_id, "Sets")  -- -> Terraria.ID.ItemID.Sets
> local dep     = mod.patch.get_field(sets, "Deprecated")
> ```

### 字段读写

```lua
mod.patch.get_field_value(field, instance[, type])       -- instance 为 nil 表示静态字段
mod.patch.set_field_value(field, instance, value[, type])
```

`type` 可省略（由内核查），但**建议显式写**，Android 上**必须写**（走真实指针，类型不明确会读错）。

可用类型名：`void` `int8` `int16` `int32` `int64` `uint8` `uint16` `uint32` `uint64`
`bool` `float` `double` `pointer` `object` `char`，以及伪类型 `string`。

结构体（如 `Vector2`，即两个 `float` 开头）：

```lua
local x, y = mod.patch.get_field_vec2(field, instance)
mod.patch.set_field_vec2(field, instance, x, y)
```

任意原始字节（长度 1~16，可配合 `string.pack`/`string.unpack`）：

```lua
local raw = mod.patch.get_field_raw(field, instance[, size])  -- 省略 size 时按字段类型推断，失败返回 nil
mod.patch.set_field_raw(field, instance, raw)                 -- raw 长度需为 1~16 字节
```

### 数组

```lua
local n   = mod.patch.array_length(array)
local obj = mod.patch.array_at(array, i)              -- 省略类型时按对象(指针)处理
local num = mod.patch.array_at(array, i, "float")     -- 值类型数组需给类型名
```

对象数组（如 `Main.npc`）的句柄来自静态字段：

```lua
local arr = mod.patch.get_field_value(mod.patch.get_field(Main, "npc"), nil, "object")
```

写入 / 创建 / 批量：

```lua
mod.patch.array_set(array, i, value[, type])   -- 写元素，成功返回 true
mod.patch.array_fill(array, value[, type])     -- 填充所有元素
mod.patch.array_create(size, elem_type)        -- 新建托管数组，elem_type 为类型句柄或类型名
```

- `array_set` 的 `type` 省略时按 Lua 值推断（`bool` / 整数→`int32` / 浮点→`float` /
  字符串→托管 string / lightuserdata→`object`）；**值类型数组建议显式给类型**（如 `"float"`、`"int32"`）；
- 内联结构体数组 / 需要按原始字节读写的场景：

```lua
local raw = mod.patch.array_at_raw(array, i, size)  -- 返回原始字节串
mod.patch.array_set_raw(array, i, raw)              -- 按字节写回
```

### 属性桥接

`get_property` 返回的属性句柄本身不可调用，需先取访问器：

```lua
local prop   = mod.patch.get_property(type, "SomeProp")
local getter = mod.patch.property_get_method(prop)
local setter = mod.patch.property_set_method(prop)
local v = mod.patch.invoke(getter, instance)   -- 读
mod.patch.invoke(setter, instance, newValue)  -- 写（参数类型取自方法签名）
```

### C 快通道（内存 / 指针，Android）

一套**通用的内存/指针原语**，把“逐元素跨 C 边界（甚至逐个托管 invoke）”压成“批量一次读”。
凡数据在**平坦内存布局**里都能用，**不限于世界 Tile**：

- 原生指针数组（`static T*`，如 `TileData.TileLookup/TileType/TileFrameX` …）；
- 内联结构体数组（如 `Recipe.requiredItemQuickLookup`、`Projectile.ai`）；
- 成批对象指针（如 `Main.npc / Main.projectile`，一次读一批句柄）；
- 任意“基址 + 连续布局”的缓冲（纹理像素、自定义二进制数据等）。

```lua
local ptr = mod.patch.field_pointer(field, instance)  -- 仅 Android 返回真实指针，桌面端为 nil
local raw = mod.patch.mem_read(ptr, offset, size)     -- 读原始字节
mod.patch.mem_write(ptr, offset, raw)                 -- 写原始字节

mod.patch.ptr_add(ptr, byte_offset)                        -- 指针偏移 -> lightuserdata
mod.patch.ptr_deref(ptr, byte_offset)                      -- 读取指针值（指针链）-> lightuserdata
mod.patch.mem_read_values(ptr, byte_offset, count, type)   -- 批量读 -> 值表（一次跨边界）
mod.patch.mem_write_values(ptr, byte_offset, table, type)  -- 批量写 -> count
mod.patch.field_size(field)                                -- 字段字节大小（不可用返回 0）
```

**示例：世界 Tile 快通道**（Tile 只是上面的一个场景；Android 上 `TileData` 是裸数组）

```lua
-- 只做一次：拿到静态字段「存储地址」，之后双重解引用（换世界时指针会变）
local p_lookup = mod.patch.field_pointer(mod.patch.get_field(TileData, "TileLookup"), nil) -- uint**
local p_type   = mod.patch.field_pointer(mod.patch.get_field(TileData, "TileType"),   nil) -- ushort*

-- 读一个坐标：GetTileType(x,y) = TileType[TileLookup[y*maxX+x]]
local lookup = mod.patch.ptr_deref(p_lookup, 0)            -- uint*  基址
local types  = mod.patch.ptr_deref(p_type,   0)            -- ushort* 基址
local idx = mod.patch.mem_read_values(lookup, (y*maxX+x)*4, 1, "uint32")[1]
if idx ~= 0xFFFFFFFF then
    local t = mod.patch.mem_read_values(types, idx*2, 1, "uint16")[1]
end

-- 批量读一段（一次跨边界）：
local idxs = mod.patch.mem_read_values(lookup, startByte, count, "uint32")
```

> 桌面端 `field_pointer` 返回 `nil`，Mod 应自动退回托管路径（`array_at` / `Framing.GetTileSafely` 等），
> 从而保持**一份脚本全平台**。裸指针偏移写错会崩游戏，请自行做好边界检查。

### 托管字符串

`System.String` 可以当 Lua 字符串读写，把类型写成 `"string"`：

```lua
local name = mod.patch.get_field_value(field, instance, "string")  -- 返回 Lua 字符串
mod.patch.set_field_value(field, instance, "你好", "string")
local s = mod.patch.array_at(array, i, "string")
```

需要手动构造/检查时：

```lua
local h = mod.patch.string_create("hello")  -- 句柄由 Lua GC 管理
mod.patch.string_value(h)                   -- "hello"（也可直接传普通 Lua 字符串）
mod.patch.string_empty(h)                   -- false
mod.patch.string_length(h)                  -- 5
```

### 创建实例

```lua
local obj  = mod.patch.new_instance(type)          -- 无参
local obj2 = mod.patch.new_instance(type, 1, "x")  -- 按 ".ctor" 参数个数自动匹配构造函数
local ctor = mod.patch.get_method(type, ".ctor", 2)
local obj3 = mod.patch.construct(ctor, 1, "x")     -- 用显式构造函数
```

### 调用方法

```lua
local r = mod.patch.invoke(method, [instance,] ...)
```

- **实例方法**：`invoke(method, instance, 实参...)`，第 2 个参数是接收者。
- **静态方法**：`invoke(method, 实参...)`，**不传 instance**。内核按方法签名自动区分；
  也可用 `method_is_instance(m)` / `method_is_static(m)` 提前判断。

```lua
-- 实例方法 Player.Heal(int)
mod.patch.invoke(heal, player, 20)
-- 静态方法 Main.NewText(...)
mod.patch.invoke(new_text, str, 255, 255, 255)
```

返回值：引用类型返回一个**借用句柄**（仅当前调用内有效，要跨帧缓存请 `retain`），
`void` 返回 `nil`，调用失败返回 `false`。

### Hook

```lua
local hook = mod.patch.install_hook(method, {
    -- 原方法执行前：返回 true 表示跳过原方法（可选再返回一个值作为返回值）
    prefix = function(instance, args, result) return false end,
    -- 原方法执行后：result 是原方法的返回值；默认忽略返回值
    postfix = function(instance, args, result) end,
    -- 需要 postfix 覆盖返回值时显式开启（默认关闭，保证兼容）：
    -- override_result = true,
})

hook:remove()   -- 手动卸载
```

- `args` 是 1 起始的参数数组；`instance` 是对象实例（静态方法为 `nil`）。
- `prefix` / `postfix` 至少写一个；最多 **1024** 个钩子（全加载器共享，释放后可复用）。
- 钩子装上后一直有效，直到 `hook:remove()` 或 Mod 卸载（丢弃返回值不会导致失效）。
- `{ copy = true }`：把 `instance`、对象参数、对象返回值包成 **GC 托管副本**，可安全跨帧缓存
  （见下）。默认不开，行为与不加时完全一致。
- `{ override_result = true }`（或 `result = true`）：允许 `postfix` 的返回值**覆盖原方法返回值**
  （类型需与签名匹配，返回 `nil` 表示不修改）。默认关闭。

### 句柄与生命周期

普通 API 返回的句柄是**借用的**，只在当前调用内有效——尤其 hook 里的 `instance` / 对象参数，
回调结束后会被内核释放。**不要把它们直接存进表里跨帧使用。**

需要跨帧缓存时，用 `retain` 拿到 **GC 托管副本**（不必手动 `free`）：

```lua
local npc = mod.patch.retain(instance)   -- 可以安全地存起来
```

或者给 hook 加 `copy = true`：

```lua
mod.patch.install_hook(method, { postfix = on_ai, copy = true })
```

托管副本只是同一对象的另一个句柄，读写字段仍然作用于原对象；Lua GC 在不再引用时自动释放。

---

## 调试与热重载

打开调试模式后可用 `/reload` 热重载。配置在**加载器私有目录**（Mod 包里看不到）：

```
<工作目录>/modloader/private/lzup333.lualoader/config.json
```

```json
{ "debug": false }
```

改成 `true` 并重启游戏后，在**游戏聊天框**输入 `/reload`：

- 该指令会重新扫描 `enables.txt` 并同步：
  - **热加载**新启用的 Mod；
  - 热卸载已禁用的 Mod；
  - 热重载正在运行的 Mod（重跑脚本与 `setup`，并重新安装钩子）；
- 重载在**下一帧**执行，避免在钩子链里销毁 Lua 状态；
- 管理器的启用/禁用是异步落盘的，切换后等几秒再 `/reload`。

改脚本 → 保存 → `/reload` 即可生效，无需重新编译或重启游戏。

---

## 常见陷阱

- **Android 一定要显式写字段类型**（如 `"int32"`），否则可能读错。
- **不要在 hook 里跨帧缓存 `instance`**（会悬垂）；要缓存用 `retain` 或 `copy = true`。
- **想修改原对象**，直接对原 `instance` 读写即可；`copy`/`retain` 得到的是同对象的另一个句柄，
  一样能改。
- Hook 时机：想让你的改动**不被原版每帧逻辑覆盖**，用 `postfix`；想**拦截/替换**，用 `prefix`
  返回 `true`。
- 字段/方法可能因游戏版本差异缺失，取不到时记得判空再继续。

---

## 完整示例

每帧把玩家魔力补满（Hook `Player.ResetEffects`）：

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

---

## 相关文档

- 原生模块（C/C++ 扩展）：[`native.md`](native.md)
