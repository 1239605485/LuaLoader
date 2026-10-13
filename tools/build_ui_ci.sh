#!/usr/bin/env bash
set -euo pipefail
build_target="${1:?target required}"
args=(-S . -B build/ci -DCMAKE_BUILD_TYPE=Release)
case "$build_target" in
  android-arm64|android-arm)
    abi=arm64-v8a
    if [[ "$build_target" == android-arm ]]; then abi=armeabi-v7a; fi
    args+=("-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" "-DANDROID_ABI=$abi" -DANDROID_PLATFORM=android-23)
    ;;
  linux-x64) args+=(-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++) ;;
  linux-x86) args+=(-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SYSTEM_PROCESSOR=i686 -DCMAKE_SYSTEM_NAME=Linux) ;;
  windows-x64) args+=(-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres) ;;
  windows-x86) args+=(-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc-posix -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++-posix -DCMAKE_RC_COMPILER=i686-w64-mingw32-windres) ;;
  *) echo "Unknown target: $build_target" >&2; exit 2 ;;
esac
cmake "${args[@]}"
cmake --build build/ci -j 4
