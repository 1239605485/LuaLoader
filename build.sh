#!/usr/bin/env bash
# =============================================================================
# build.sh —— LuaLoader 构建 / 打包脚本
#
# 一条命令完成：编译各平台 → （可选）用 Python 版 tefpkg_tool 打包 .tefpkg
#              → 与 Info.json / Manifest.json 一起压成发行 zip。
#
# 用法：
#   ./build.sh [平台...] [选项]
#
# 平台（默认全部 6 个）：
#   linux-x64  linux-x86  win-x64  win-x86  android-arm64  android-arm
#   也可写 all
#
# 选项：
#   -r, --release          Release 构建（默认）
#   -d, --debug            Debug 构建
#   -c, --clean            配置前清空对应 build 目录
#   -j, --jobs N           并行数（默认 nproc）
#       --gui              （已弃用）原生 GUI 现为内置，无需开关
#       --implib           把 Windows 导入库复制到 sdk/windows_<arch>/
#       --package          构建后打包 dist/lualoader.tefpkg 并生成发行 zip
#       --package-only     跳过编译，仅用现有产物打包
#   -f, --fingerprint HEX  tefpkg 指纹（默认 0x114514）
#       --kernel-lib PATH  打包用的 libtefkernel*.so（不扫盘；也可设 TEFKERNEL_LIB）
#       --no-zip           打包 tefpkg 但不生成发行 zip
#   -n, --dry-run          只打印将执行的命令
#   -h, --help             显示本帮助
#
# 产物：
#   build/rel-<平台>/libloader.<系统>.<架构>.{so,dll}
#   dist/lualoader.tefpkg              （--package）
#   dist/LuaLoader-v<版本>.zip         （--package，含 tefpkg + Info.json + Manifest.json）
#
# 说明：
#   - 打包走 tools/tefpkg_tool.py（Python 版，ctypes 直接调 TEFKernel 的 tefpkg API），
#     不依赖那份被改过的 C++ TEFPkg-Tool。
#   - 内核库（打包用）与 Android NDK 都不自动扫描磁盘，需显式指定：
#       TEFKERNEL_LIB / --kernel-lib ，以及 ANDROID_NDK_HOME。
# =============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# ---------- 颜色 ----------
if [ -t 1 ]; then
  B=$'\033[1m'; G=$'\033[32m'; Y=$'\033[33m'; RED=$'\033[31m'; D=$'\033[2m'; R=$'\033[0m'
else
  B=""; G=""; Y=""; RED=""; D=""; R=""
fi
info() { printf '%s\n' "${B}$*${R}"; }
warn() { printf '%s\n' "${Y}$*${R}" >&2; }
err()  { printf '%s\n' "${RED}$*${R}" >&2; }
run()  { if [ "$DRY" = 1 ]; then printf '%s\n' "${D}+ $*${R}"; else "$@"; fi; }

# ---------- 参数 ----------
ALL_PLATFORMS="linux-x64 linux-x86 win-x64 win-x86 android-arm64 android-arm"
PLATFORMS=()
BT=Release
CLEAN=0; JOBS="$(nproc 2>/dev/null || echo 4)"
GUI=0; IMPLIB=0; PACKAGE=0; PACKAGE_ONLY=0; NO_ZIP=0; DRY=0
FP=0x114514
KERNEL_LIB="${TEFKERNEL_LIB:-}"

usage() { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    -r|--release) BT=Release; shift ;;
    -d|--debug)   BT=Debug; shift ;;
    -c|--clean)   CLEAN=1; shift ;;
    -j|--jobs)    JOBS="$2"; shift 2 ;;
    --gui)        GUI=1; shift ;;
    --implib)     IMPLIB=1; shift ;;
    --package)    PACKAGE=1; shift ;;
    --package-only) PACKAGE_ONLY=1; PACKAGE=1; shift ;;
    -f|--fingerprint) FP="$2"; shift 2 ;;
    --kernel-lib)   KERNEL_LIB="$2"; shift 2 ;;
    --no-zip)     NO_ZIP=1; shift ;;
    -n|--dry-run) DRY=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    all)          PLATFORMS+=($ALL_PLATFORMS); shift ;;
    linux-x64|linux-x86|win-x64|win-x86|android-arm64|android-arm)
                  PLATFORMS+=("$1"); shift ;;
    -*)           err "未知参数: $1"; usage; exit 2 ;;
    *)            err "未知平台: $1"; usage; exit 2 ;;
  esac
done
[ ${#PLATFORMS[@]} -eq 0 ] && PLATFORMS=($ALL_PLATFORMS)

# ---------- 平台映射 ----------
builddir_of() {
  case "$1" in
    linux-x64)     echo build/rel-linux-x64 ;;
    linux-x86)     echo build/rel-linux-x86 ;;
    win-x64)       echo build/rel-win-x64 ;;
    win-x86)       echo build/rel-win-x86 ;;
    android-arm64) echo build/rel-android-arm64 ;;
    android-arm)   echo build/rel-android-arm32 ;;
  esac
}
artifact_of() {
  case "$1" in
    linux-x64)     echo libloader.linux.x64.so ;;
    linux-x86)     echo libloader.linux.x86.so ;;
    win-x64)       echo libloader.windows.x64.dll ;;
    win-x86)       echo libloader.windows.x86.dll ;;
    android-arm64) echo libloader.android.arm64.so ;;
    android-arm)   echo libloader.android.arm.so ;;
  esac
}

# ---------- Android NDK（不扫盘，只认用户显式设置） ----------
find_ndk() {
  [ -n "${ANDROID_NDK_HOME:-}" ] && [ -d "$ANDROID_NDK_HOME" ] && { echo "$ANDROID_NDK_HOME"; return; }
  echo ""
}

# ---------- 配置 + 编译 ----------
cmake_configure() {
  local p="$1" dir="$2"; shift 2
  local args=(-S "$ROOT" -B "$dir" -G "Unix Makefiles" "-DCMAKE_BUILD_TYPE=$BT")
  args+=("$@")
  run cmake "${args[@]}"
}

build_platform() {
  local p="$1" dir; dir="$(builddir_of "$p")"
  info "──────── 构建 $p ($BT) ────────"
  [ "$CLEAN" = 1 ] && run rm -rf "$dir"

  local ndk=""
  case "$p" in
    linux-x64)
      cmake_configure "$p" "$dir" -DANDROID_MODE=OFF \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ;;
    linux-x86)
      cmake_configure "$p" "$dir" -DANDROID_MODE=OFF \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchains/linux-x86.cmake" ;;
    win-x64)
      cmake_configure "$p" "$dir" -DCMAKE_SYSTEM_NAME=Windows -DANDROID_MODE=OFF \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ -DTARGET=X86_WIN64 ;;
    win-x86)
      cmake_configure "$p" "$dir" -DCMAKE_SYSTEM_NAME=Windows -DANDROID_MODE=OFF \
        -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
        -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ -DTARGET=X86_WIN32 ;;
    android-arm64|android-arm)
      ndk="$(find_ndk)"
      if [ -z "$ndk" ]; then
        err "跳过 $p：未设置 ANDROID_NDK_HOME（不自动扫描磁盘）"; return 1
      fi
      local abi=arm64-v8a
      [ "$p" = android-arm ] && abi=armeabi-v7a
      ANDROID_NDK_HOME="$ndk" cmake_configure "$p" "$dir" -DANDROID_MODE=ON \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_NDK="$ndk" ;;
  esac

  run cmake --build "$dir" -j "$JOBS"
}

copy_implib() {
  local p="$1" dir; dir="$(builddir_of "$p")"
  case "$p" in
    win-x64) run mkdir -p "$ROOT/sdk/windows_x64"; run cp -f "$dir/libloader.windows.x64.dll.a" "$ROOT/sdk/windows_x64/" ;;
    win-x86) run mkdir -p "$ROOT/sdk/windows_x86"; run cp -f "$dir/libloader.windows.x86.dll.a" "$ROOT/sdk/windows_x86/" ;;
  esac
}

# ---------- 打包 ----------
package() {
  local stage="$ROOT/build/tefpkg-stage"
  local tool="$ROOT/tools/tefpkg_tool.py"
  local tefpkg="$ROOT/dist/lualoader.tefpkg"
  if [ -n "$KERNEL_LIB" ]; then
    [ -f "$KERNEL_LIB" ] || { err "指定的内核库不存在：$KERNEL_LIB"; return 1; }
    export TEFKERNEL_LIB="$KERNEL_LIB"
  fi
  if [ -z "${TEFKERNEL_LIB:-}" ]; then
    err "打包需要内核库：请用 --kernel-lib <libtefkernel*.so> 或设 TEFKERNEL_LIB（不自动扫描）"
    return 1
  fi
  info "──────── 打包 tefpkg ────────"
  run rm -rf "$stage"; run mkdir -p "$stage" "$ROOT/dist"
  local missing=0
  for p in $ALL_PLATFORMS; do
    local art dir
    dir="$(builddir_of "$p")"; art="$(artifact_of "$p")"
    if [ -f "$dir/$art" ]; then
      run cp -f "$dir/$art" "$stage/"
    else
      warn "缺少 $dir/$art（该平台在包内将留空占位）"; missing=1
    fi
  done
  run python3 "$tool" build "$stage" "$tefpkg" "$FP" -c "$ROOT/tools/compress_loader.json" || return 1

  [ "$NO_ZIP" = 1 ] && { info "已生成 $tefpkg（--no-zip，跳过发行 zip）"; return 0; }

  local ver
  ver="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/Info.json" | head -1)"
  [ -n "$ver" ] || ver=0.0.0
  local zip="$ROOT/dist/LuaLoader-v$ver.zip"
  info "──────── 生成发行 zip ────────"
  run rm -f "$zip"
  run zip -qrX "$zip" -j "$tefpkg" "$ROOT/Info.json" "$ROOT/Manifest.json" || return 1
  info "完成：$zip"
  run unzip -l "$zip"
}

# ---------- 主流程 ----------
info "LuaLoader 构建脚本   ROOT=$ROOT"
info "构建类型：$BT   平台：${PLATFORMS[*]}   并行：$JOBS   指纹：$FP"

if [ "$PACKAGE_ONLY" != 1 ]; then
  FAILED=()
  for p in "${PLATFORMS[@]}"; do
    if ! build_platform "$p"; then FAILED+=("$p"); fi
    case "$p" in win-*) [ "$IMPLIB" = 1 ] && copy_implib "$p" ;; esac
  done
  # 默认也刷新一下 Windows 导入库（若刚构建过 win）
  for p in "${PLATFORMS[@]}"; do
    case "$p" in win-x64|win-x86) copy_implib "$p" ;; esac
  done
  if [ ${#FAILED[@]} -gt 0 ]; then
    err "以下平台构建失败：${FAILED[*]}"
  fi
fi

if [ "$PACKAGE" = 1 ]; then
  package || { err "打包失败"; exit 1; }
fi

info "全部完成。"
