# 原生模块（C/C++ 扩展）

Mod 可自带原生库（`.so` / `.dll`），用于性能敏感或 Lua 难实现的场景。纯 Lua Mod 不受影响。

## 目录约定

```
Resources/native/<平台>_<架构>/<模块名>.so      # 首选：linux_x64 / android_arm64 / windows_x64 ...
Resources/native/<平台>/<模块名>.so             # 次选（不区分架构）
```

部署后加载器把命中的目录加入 `package.cpath`，并以 `RTLD_GLOBAL` 预加载。

## Lua 侧

标准 C 模块，`require` 即用：

```lua
local hello = require("hello")   -- 找 hello.so，调 luaopen_hello
print(hello.greet("world"))
```

## C 侧

Android 上 loader 以 `RTLD_LOCAL` 加载，`lua_*` 无法从 ELF 全局解析，统一用注入的 API 表
（桌面端可直接 `extern`）：

```c
#include "lualoader_mod.h"

static const ll_api_t *LL = NULL;
LL_EXPORT void ll_set_api(const ll_api_t *api) { LL = api; }   /* loader 注入 */

int luaopen_hello(lua_State *L) {
    if (!LL) return 0;                                          /* 旧 loader 优雅失败 */
    LL_CACHE(p_pushstring, lua_pushstring, void, (lua_State *, const char *));
    p_pushstring(L, "hello");
    return 1;
}
```

- `LL->lookup("名字")` 解析 loader 导出的 `lua_*` / `luaL_*`，以及 `patchlib_*` 函数指针变量（自动解引用）。
- `LL_CACHE(变量, 符号名, 返回类型, (参数类型...))` 首次解析并缓存。

> 部分常用名是**宏**、没有导出符号，需换底层函数：`lua_pushcfunction`→`lua_pushcclosure`
> （补 0）、`lua_tostring`→`lua_tolstring`（补 NULL）、`lua_newtable`→`lua_createtable`、
> `lua_pop`→`lua_settop`、`luaL_newlib`→`luaL_checkversion + luaL_newlibtable + luaL_setfuncs` 等。

## 编译

```bash
INC=-I<LuaLoader>/includes
clang -O2 -fPIC -shared -o hello.so hello.c $INC        # Linux
$NDK/.../aarch64-linux-android24-clang -O2 -fPIC -shared -o hello.so hello.c $INC   # Android
x86_64-w64-mingw32-gcc -O2 -shared -o hello.dll hello.c $INC <SDK>/windows_x64/libloader.windows.x64.dll.a   # Windows
```

Windows 导入库不随源码仓库分发，请从 Releases 下载 `LuaLoader-sdk-v<版本>.zip`，
解压后把其中的 `sdk/` 目录当作上面的 `<SDK>`。需要自行生成时：
`./build.sh win-x64 win-x86 --implib --sdk-zip`。

打包即普通 Mod 目录：`Resources/lib/main.lua` + `Resources/native/<平台>_<架构>/hello.so`。

## 排查

- 预加载成功/失败日志：`preloaded native module` / `dlopen failed: <原因>`；
- `require` 失败返回错误字符串（符号缺失、架构不匹配等）；
- 报 `dynamic libraries not enabled` 表示内置 Lua 没开 `LUA_USE_DLOPEN`（本项目 1.6.0 起已开）。

## 安全

原生模块等于在游戏进程内执行任意原生代码，风险高于纯 Lua。请只安装信任来源的 Mod。
