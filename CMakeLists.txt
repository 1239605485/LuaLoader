cmake_minimum_required(VERSION 3.28.3)
project(LuaLoader)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
else()
    add_compile_options(-g -O0)
endif()

set(ANDROID_MODE OFF)
if (${ANDROID_MODE})
    set(ANDROID ON)
    add_compile_definitions(__ANDROID__)
    # sysroot / 工具链由 NDK 的 android.toolchain.cmake 自动设置，无需写死路径
endif ()

# Vendored dependencies
add_subdirectory(lib/spdlog-1.17.0)
add_subdirectory(lib/lua-5.4.8)

# 原生 GUI：直接使用游戏自带的 XNA/Unity SpriteBatch 绘制，不再依赖 Dear ImGui。
set(LUALOADER_SOURCES
        src/core/core.cpp
        src/core/logger.cpp
        src/lua/lua_engine.cpp
        src/api/api_common.cpp
        src/api/api_fs.cpp
        src/api/api_net.cpp
        src/api/api_patch_core.cpp
        src/api/api_patch_array.cpp
        src/api/api_patch_inspect.cpp
        src/api/api_patch_container.cpp
        src/api/api_patch_struct.cpp
        src/api/api_patch_hook.cpp
        src/api/api_item.cpp
        src/api/api_recipe.cpp
        src/api/api_projectile.cpp
        src/api/api_npc.cpp
        src/api/api_buff.cpp
        src/api/api_register.cpp
        src/core/mod_api.cpp
        src/gui.cpp
        src/gui/gui_images.cpp
        src/gui/gui_ime.cpp
        src/gui/gui_keyboard.cpp
        src/util/pinyin.cpp
        src/util/image_decode.cpp
        includes/tefkernel/tef_api_imp.c
)

add_library(LuaLoader SHARED ${LUALOADER_SOURCES})

target_include_directories(LuaLoader
        PUBLIC
        includes
        includes/tefkernel
)

target_link_libraries(LuaLoader
        spdlog::spdlog
        lua
)

# 原生 GUI 默认编译进 Loader（无需额外开关）
target_compile_definitions(LuaLoader PRIVATE LUALOADER_GUI=1)

if (MINGW)
    # 静态链接 MinGW 运行库，避免目标机缺少
    # libstdc++-6.dll / libgcc_s_*.dll / libwinpthread-1.dll 导致加载失败
    target_link_options(LuaLoader PRIVATE -static)
    # 导出全部符号（lua_* / luaL_* / patchlib_*），供原生模块 DLL 解析
    target_link_options(LuaLoader PRIVATE -Wl,--export-all-symbols)
endif()


if (CMAKE_BUILD_TYPE STREQUAL "Release")
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        add_custom_command(TARGET LuaLoader POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Stripping symbols from LuaLoader"
                COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:LuaLoader>
                COMMENT "Stripping symbols from LuaLoader library using strip command"
                VERBATIM
        )
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_custom_command(TARGET LuaLoader POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Stripping symbols from LuaLoader..."
                COMMAND ${CMAKE_STRIP} -S $<TARGET_FILE:LuaLoader>
                COMMENT "Stripping symbols from LuaLoader library using strip command"
                VERBATIM
        )
    endif()
endif()


if(ANDROID)
    set(SYSTEM_NAME "android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SYSTEM_NAME "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(SYSTEM_NAME "darwin")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SYSTEM_NAME "windows")
else()
    set(SYSTEM_NAME "unknown")
endif()

# 获取架构
if(MINGW OR WIN32)
    if(CMAKE_C_COMPILER MATCHES "x86_64-w64-mingw32" OR CMAKE_CXX_COMPILER MATCHES "x86_64-w64-mingw32")
        set(ARCH "x64")
    elseif(CMAKE_C_COMPILER MATCHES "i686-w64-mingw32" OR CMAKE_CXX_COMPILER MATCHES "i686-w64-mingw32")
        set(ARCH "x86")
    else()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(ARCH "x64")
        else()
            set(ARCH "x86")
        endif()
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(ARCH "arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7|armv7a|ARMv7|arm")
        set(ARCH "arm")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|x64")
        set(ARCH "x64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i686|i386|x86")
        set(ARCH "x86")
    else()
        set(ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
endif()

# 设置输出文件名（与 KernelLoader 保持一致的 loader.<platform>.<arch> 约定）
set_target_properties(LuaLoader PROPERTIES
    OUTPUT_NAME "loader.${SYSTEM_NAME}.${ARCH}"
)

if(ANDROID)
    add_subdirectory(lib/gui-xdl)
    set_target_properties(xdl PROPERTIES C_VISIBILITY_PRESET hidden)
    target_link_libraries(LuaLoader xdl)
endif()
