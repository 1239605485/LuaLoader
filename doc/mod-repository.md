# LuaLoader Mod 仓库客户端

LuaLoader 的 ModLoader 元数据已声明 Community-TEFKernel-LuaLoader-Mods 的
`modRepositoryIndexUrl`。更新 LuaLoader 后，TEFManager 会自动发现并显示该仓库，
用户不需要额外配置仓库地址。

LuaLoader 还提供了不改变现有 Mod/native ABI 的仓库索引客户端接口，定义在
`includes/mod_repository.hpp`。

客户端支持：

- 读取 TEFManager `ModRepositoryIndex` 格式的静态 `index.json`；
- 拒绝绝对路径、`..` 路径和原生包索引项；
- 将仓库相对路径安全解析到仓库根目录；
- 校验下载文件为 ZIP，并按索引校验 SHA-256。

包管理器负责把下载的 ZIP 内容安全解压到
`mods/<modloader-id>/mod/<pkgId>/`。LuaLoader 运行时继续从该目录读取
`luamod.json` 和 `Resources/lib/main.lua`，不会把仓库包直接交给原生模块加载路径。

默认包管理器目录：

```text
~/.config/lualoader/modrepo.json
~/.local/share/lualoader-modrepo/
~/.local/state/lualoader-modrepo/
```
