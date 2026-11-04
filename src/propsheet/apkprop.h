// `apkprop.dll` 的 COM 入口与共享声明（CLSID 一经发布不得变更）。
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

// {A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}（M1 时已在 apkthumb.h 里预留序号）
extern const CLSID CLSID_ApkPropertyPage;

// 本 DLL 句柄（worker_client 用它定位 apkinfo.exe）
extern HMODULE g_module;

// COM server 生命周期计数（与 apkthumb 各自独立：两个 DLL 分别被加载）
void IncrementServerLock();
void DecrementServerLock();
long ServerLockCount();

// 类厂创建属性页对象
HRESULT CreatePropertyPage(REFIID riid, void** ppv);

// 资源 ID（与 apkprop.rc 一致）；页面标题/文案不在这里，见 src/core/i18n.cpp
#define IDD_APKPROP 101
