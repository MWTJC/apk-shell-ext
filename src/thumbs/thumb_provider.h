#pragma once

#include <windows.h>

// 创建缩略图 provider 实例（由类厂调用；M1 的唯一实现）
HRESULT CreateThumbProvider(REFIID riid, void** ppv);
