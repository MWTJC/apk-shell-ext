// 版本号的唯一来源：VERSIONINFO（三个 .rc）、安装器的 DisplayVersion、文档里的版本行都从这里取。
// 改版本只改这里；APKSX_PARSER_VERSION（缓存键里的解析器代号）在 CMakeLists 里单独维护。
#pragma once

#define APKSX_VERSION_MAJOR 1
#define APKSX_VERSION_MINOR 0
#define APKSX_VERSION_PATCH 0
#define APKSX_VERSION_BUILD 0

#define APKSX_VERSION_STRING "1.0.0"
#define APKSX_VERSION_FULL "1.0.0.0"

// 安装器在 HKCU\Software\apk-shell-ext 里记的"安装版本"，升级检测用
#define APKSX_PRODUCT_NAME "apk-shell-ext"
