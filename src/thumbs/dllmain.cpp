#include "thumbs/apkthumb.h"

#include <windows.h>

#include "core/log.h"

// GUID 定义集中在此文件（等价于 INITGUID，但不必污染编译选项）
const CLSID CLSID_ApkThumbProvider = {0xA7C4E1B0,
                                      0x5E8D,
                                      0x4C3A,
                                      {0x9F, 0x2B, 0x6D, 0x1C, 0x8E, 0x4A, 0x7B, 0x01}};
const CLSID CLSID_ApkPropertyPage = {0xA7C4E1B0,
                                     0x5E8D,
                                     0x4C3A,
                                     {0x9F, 0x2B, 0x6D, 0x1C, 0x8E, 0x4A, 0x7B, 0x02}};

HMODULE g_module = nullptr;

namespace {

volatile long g_server_locks = 0;

}  // namespace

void IncrementServerLock() { ::InterlockedIncrement(&g_server_locks); }
void DecrementServerLock() { ::InterlockedDecrement(&g_server_locks); }
long ServerLockCount() { return g_server_locks; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    ::DisableThreadLibraryCalls(module);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv);
STDAPI DllCanUnloadNow();
