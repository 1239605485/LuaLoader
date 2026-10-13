"""Package the compiled multi-platform loader with metadata at ZIP root."""
import json
import pathlib
import zipfile
root = pathlib.Path(__file__).resolve().parent.parent
info = json.loads((root / 'Info.json').read_text(encoding='utf-8'))
expected = {'libloader.android.arm64.so', 'libloader.android.arm.so', 'libloader.linux.x64.so', 'libloader.linux.x86.so', 'libloader.windows.x64.dll', 'libloader.windows.x86.dll'}
assert expected <= {p.name for p in (root / 'build/package').iterdir()}, 'Missing platform binary'
out = root / 'dist' / ('LuaLoader-' + info['version'] + '-all-platforms.zip')
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in [root / 'Info.json', root / 'Manifest.json', root / 'dist/lualoader.tefpkg']:
        archive.write(path, path.name)
with zipfile.ZipFile(out) as archive:
    assert archive.testzip() is None
print(out)
