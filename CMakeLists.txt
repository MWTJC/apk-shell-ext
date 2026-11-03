cmake_minimum_required(VERSION 3.27)
project(apk_shell_ext LANGUAGES C CXX RC)

# ---------------------------------------------------------------------------
# 全局约定（docs/architecture.md §5、ADR-0004）
#   C++20 / MSVC v143+ / 静态 CRT / /W4 /WX / 只链接系统库
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC)
  message(FATAL_ERROR "This project targets MSVC only (see ADR-0004).")
endif()

set(APKSX_OUT_DIR "${CMAKE_SOURCE_DIR}/out/x64")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${APKSX_OUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${APKSX_OUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${APKSX_OUT_DIR}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${APKSX_OUT_DIR}")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${APKSX_OUT_DIR}")
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${APKSX_OUT_DIR}")
endforeach()

add_compile_options(
  /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor
  /guard:cf
)
add_link_options(/DYNAMICBASE /NXCOMPAT /guard:cf /INCREMENTAL:NO)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# 第三方：miniz（仅用作 raw deflate 解压；MIT，单文件）
#   只编译需要的部分，并关闭 /WX——第三方代码不参与本项目的告警门槛。
# ---------------------------------------------------------------------------
add_library(apksx_miniz STATIC src/third_party/miniz/miniz.c)
target_include_directories(apksx_miniz PUBLIC src/third_party/miniz)
target_compile_definitions(apksx_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_WRITING_APIS
                                             MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_options(apksx_miniz PRIVATE /W3 /WX- /wd4244 /wd4267 /wd4996)

# ---------------------------------------------------------------------------
# 解析核心（全部静态链接进 DLL 与 worker；不得依赖托管/第三方运行时）
# ---------------------------------------------------------------------------
add_library(apksx_core STATIC
  src/core/file_util.cpp
  src/core/log.cpp
  src/core/string_pool.cpp
  src/core/zip_reader.cpp
  src/core/byte_source.cpp
  src/core/axml.cpp
  src/core/arsc.cpp
  src/core/arsc_stream.cpp
  src/core/apk_icon.cpp
  src/core/apk_info.cpp
  src/core/wic_image.cpp
  src/core/thumb_cache.cpp
)
target_include_directories(apksx_core PUBLIC src)
target_link_libraries(apksx_core PUBLIC apksx_miniz
  bcrypt windowscodecs ole32 shlwapi)
target_compile_definitions(apksx_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN
                                               NOMINMAX APKSX_PARSER_VERSION="m1.0")

# ---------------------------------------------------------------------------
# apkthumb.dll —— 唯一进入 explorer 的代码：IThumbnailProvider
# ---------------------------------------------------------------------------
add_library(apkthumb SHARED
  src/thumbs/dllmain.cpp
  src/thumbs/class_factory.cpp
  src/thumbs/thumb_provider.cpp
  src/thumbs/apkthumb.def
)
target_link_libraries(apkthumb PRIVATE apksx_core shell32 ole32)
target_compile_definitions(apkthumb PRIVATE APKSX_THUMB_EXPORTS)
set_target_properties(apkthumb PROPERTIES OUTPUT_NAME "apkthumb" PREFIX "")
if(MSVC)
  target_sources(apkthumb PRIVATE src/thumbs/apkthumb.rc)
endif()

# ---------------------------------------------------------------------------
# apkinfo.exe —— 全量解析 worker（ADR-0003）。无 UI，stdout 出 JSON。
# ---------------------------------------------------------------------------
add_executable(apkinfo src/worker/apkinfo.cpp)
target_link_libraries(apkinfo PRIVATE apksx_core)
set_target_properties(apkinfo PROPERTIES OUTPUT_NAME "apkinfo")

# ---------------------------------------------------------------------------
# 测试工具：thumbtest.exe（直接驱动 provider、批量计时、坏样本回归）
# ---------------------------------------------------------------------------
add_executable(thumbtest tests/thumbtest.cpp)
target_link_libraries(thumbtest PRIVATE apksx_core shell32 ole32 windowscodecs shlwapi)
set_target_properties(thumbtest PROPERTIES OUTPUT_NAME "thumbtest")

# 安装部署（M5 会换成安装器；此处供 register.ps1 复制产物）
install(TARGETS apkthumb thumbtest apkinfo RUNTIME DESTINATION . LIBRARY DESTINATION .)
