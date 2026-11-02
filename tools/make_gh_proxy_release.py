#!/usr/bin/env python3
"""Create the optional LuaLoader package that downloads Mods through gh-proxy."""

from __future__ import annotations

import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"
PROXY_INDEX = (
    "https://gh-proxy.com/"
    "https://raw.githubusercontent.com/lzup333/Community-TEFKernel-LuaLoader-Mods/main/"
    "index-gh-proxy.json"
)
WARNING = (
    "[gh-proxy镜像版] Mod 仓库索引和 Mod 下载通过第三方 gh-proxy 镜像服务。"
    "该服务的可用性、稳定性和安全性不保证，请仅在确认信任该服务后使用。"
)


def main() -> int:
    info = json.loads((ROOT / "Info.json").read_text(encoding="utf-8"))
    manifest = json.loads((ROOT / "Manifest.json").read_text(encoding="utf-8"))
    package = DIST / "lualoader.tefpkg"
    if not package.is_file():
        raise SystemExit(f"missing package: {package}")

    mirror_info = dict(info)
    mirror_info["description"] = f"{info.get('description', '')}\n{WARNING}"
    mirror_info["modRepositoryIndexUrl"] = PROXY_INDEX

    mirror_manifest = dict(manifest)
    mirror_manifest["modRepositoryIndexUrl"] = PROXY_INDEX

    output = DIST / f"LuaLoader-v{info['version']}-gh-proxy.zip"
    with ZipFile(output, "w", ZIP_DEFLATED) as archive:
        archive.writestr("Info.json", json.dumps(mirror_info, ensure_ascii=False, indent=2) + "\n")
        archive.writestr("Manifest.json", json.dumps(mirror_manifest, ensure_ascii=False, indent=2) + "\n")
        archive.write(package, "lualoader.tefpkg")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
