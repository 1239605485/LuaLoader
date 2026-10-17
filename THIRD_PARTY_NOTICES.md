# 第三方组件与许可证说明 (Third-Party Notices)

LuaLoader 本体以 **AGPL-3.0-or-later** 发布，完整许可见 [`LICENSE`](LICENSE)。

本文件列出随仓库分发的第三方组件、版本、许可证与来源。所有许可证**原文**见 [`LICENSES/`](LICENSES/)。

## 组件清单

| 组件 | 版本 | 许可证 | 版权 | 仓库位置 | 用途 |
| --- | --- | --- | --- | --- | --- |
| Lua | 5.4.8 | MIT | © 1994-2025 Lua.org, PUC-Rio | `lib/lua-5.4.8/` | 内嵌 Lua 脚本引擎 |
| spdlog | 1.17.0 | MIT | © 2016-present Gabi Melman and spdlog contributors | `lib/spdlog-1.17.0/` | 日志 |
| {fmt}（spdlog 内置） | 12.1.0 | MIT | © 2012-present Victor Zverovich and {fmt} contributors | `lib/spdlog-1.17.0/include/spdlog/fmt/bundled/` | spdlog 的格式化依赖 |
| JSON for Modern C++（nlohmann/json） | 3.12.0 | MIT | © 2013-2025 Niels Lohmann | `includes/json.hpp` | JSON 解析 |
| TEFKernel 头文件 | — | 头文件为 **MIT**（例外见下）；TEFKernel 仓库本体为 **AGPL-3.0** | © 2025 eternalfuture-e38299 | `includes/tefkernel/` | 补丁 / 内核 API 声明 |
| KernelLoader `mod_logger.h` | — | **MIT** | © 2026 eternalfuture-e38299 | `mod-api/` | Mod 日志桥接 |
| stb_image | 2.30 | **MIT 或 Public Domain（Unlicense）双许可，任选其一** | © 2017 Sean Barrett | `includes/vendor/stb_image.h` | PNG 解码（仅启用 PNG） |
| xDL | 2.3.0 | **MIT** | © 2020-2024 HexHacking Team | `lib/gui-xdl/` | Android 符号查找回退（`xdl_open` 等） |
| rime-pinyin-simp（袖珍简化字拼音） | 0.1 | **Apache-2.0** | © rime 及 AOSP PinyinIME 作者（见 `lib/ime/AUTHORS-rime-pinyin-simp.txt`） | `lib/ime/pinyin_simp.dict.yaml` | 内置拼音输入法词库（字+词，带权重） |

## 许可证原文

| 文件 | 组件 | 来源 |
| --- | --- | --- |
| `LICENSES/AGPL-3.0.txt` | LuaLoader 本体 / TEFKernel(AGPL 部分) | <https://www.gnu.org/licenses/agpl-3.0.txt> |
| `LICENSES/MIT-lua.txt` | Lua 5.4.8 | `lib/lua-5.4.8/src/lua.h`（随附源文件，版本匹配） |
| `LICENSES/MIT-spdlog.txt` | spdlog v1.17.0 | <https://raw.githubusercontent.com/gabime/spdlog/v1.17.0/LICENSE> |
| `LICENSES/MIT-fmt.txt` | {fmt} 12.1.0 | <https://raw.githubusercontent.com/fmtlib/fmt/12.1.0/LICENSE> |
| `LICENSES/MIT-json.txt` | nlohmann/json v3.12.0 | <https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT> |
| `LICENSES/MIT-tefkernel.txt` | TEFKernel 头文件 | `includes/tefkernel/tef_api.h`（随附头文件） |
| `LICENSES/MIT-kernelloader.txt` | KernelLoader `mod_logger.h` | `mod-api/mod_logger.h`（随附头文件） |
| `LICENSES/stb_image.txt` | stb_image 2.30 | <https://raw.githubusercontent.com/nothings/stb/master/LICENSE> |
| `LICENSES/MIT-xdl.txt` | xDL 2.3.0 | <https://raw.githubusercontent.com/hexhacking/xDL/master/LICENSE> |
| `LICENSES/Apache-2.0-rime-pinyin-simp.txt` | rime-pinyin-simp（袖珍简化字拼音） | <https://github.com/rime/rime-pinyin-simp/blob/master/LICENSE> |

## 说明

- 各 MIT 组件均要求在**任何复制/分发中保留其版权声明与许可原文**；本仓库的 `LICENSES/` 即为该保留。
- **TEFKernel 为多协议**：仓库整体为 **AGPL-3.0**；随 `includes/tefkernel/` 分发的头文件按其**文件头**为 **MIT**。
  例外：`includes/tefkernel/tefpackage/tefpkg.h` 标注为 **AGPL-3.0**。MIT 与 AGPL 均与 LuaLoader 的
  AGPL-3.0 兼容。
- **KernelLoader**：`mod-api/mod_logger.h` 来自 KernelLoader（作者同为 eternalfuture-e38299），文件头标注 **MIT**。
- 本项目在链接/合并 AGPL 组件时，遵循 GNU AGPL-3.0 的相关条款（含第 13 条）。
- 若对上述任一组件做过本地修改，请在分发时保留原始许可声明并说明修改内容。
- 组件升级时，请同步更新本文件与 `LICENSES/`，并核对版本号与上游许可原文。
- **stb_image**：vendored 于 `includes/vendor/stb_image.h`（上游 <https://github.com/nothings/stb>），双许可 MIT / Public Domain 任选其一；本项目仅启用 PNG。文件 SHA-256：`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`。
- **xDL**：位于 `lib/gui-xdl/`，仅用于 Android 符号查找回退；版权与 MIT 原文见 `LICENSES/MIT-xdl.txt`。
- **rime-pinyin-simp**：位于 `lib/ime/`，为内置拼音输入法的词库（字+词，带权重）。
  上游 `rime/rime-pinyin-simp` 以 **Apache-2.0** 发布；其 `pinyin_simp.dict.yaml` 派生自
  AOSP `PinyinIME`（Apache-2.0）。版权与许可原文见 `LICENSES/Apache-2.0-rime-pinyin-simp.txt`
  与 `lib/ime/AUTHORS-rime-pinyin-simp.txt`。Apache-2.0 与本项目的 AGPL-3.0 兼容。

> 本说明不构成法律意见；如需正式合规审查，请咨询专业人士。
