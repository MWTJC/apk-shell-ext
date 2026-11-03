// apkthumb.dll 的 CLSID 与共享声明（CLSID 一经发布不得变更，写进 README/ADR）。
#pragma once

#include <windows.h>
#include <thumbcache.h>

// {A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}
extern const CLSID CLSID_ApkThumbProvider;
// {A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}  （M3 的属性页，先在 M1 预留序号）
extern const CLSID CLSID_ApkPropertyPage;

// explorer 里加载的 DLL 句柄（供 DllCanUnloadNow 与诊断使用）
extern HMODULE g_module;

// 全局对象计数（COM server 生命周期）
void IncrementServerLock();
void DecrementServerLock();
long ServerLockCount();
