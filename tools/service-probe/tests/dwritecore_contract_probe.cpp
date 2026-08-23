#include <windows.h>
#include <dwrite.h>

#include <iostream>
#include <string>

namespace {

using CreateFactory = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
using RawFactoryAddress = FARPROC(WINAPI*)();
struct NativeUnicodeString {
  USHORT length;
  USHORT maximum_length;
  PWSTR buffer;
};
using NativeLoadLibrary = LONG(NTAPI*)(PWSTR, PULONG, NativeUnicodeString*,
                                      PHANDLE);

bool WaitForLifecycleStage(const std::wstring& diagnostic_namespace,
                           const wchar_t* stage) {
  const std::wstring event_name =
      L"Local\\MacType." + diagnostic_namespace + L".pid-" +
      std::to_wstring(GetCurrentProcessId()) + L"." + stage;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  while (GetTickCount64() < deadline) {
    const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, event_name.c_str());
    if (event != nullptr) {
      const DWORD wait = WaitForSingleObject(event, 100);
      CloseHandle(event);
      if (wait == WAIT_OBJECT_0) {
        return true;
      }
    }
    Sleep(10);
  }
  return false;
}

}  // namespace

int wmain(const int count, wchar_t** values) {
  if (count != 3) {
    std::wcerr << L"usage: dwritecore-contract-probe <MacType.dll> <DWriteCore.dll>\n";
    return 64;
  }
  const std::wstring diagnostic_namespace =
      L"dwritecore-contract-" + std::to_wstring(GetCurrentProcessId());
  if (!SetEnvironmentVariableW(L"MACTYPE_DIRECTWRITE_DIAGNOSTICS",
                               diagnostic_namespace.c_str())) {
    return 1;
  }
  const HMODULE core = LoadLibraryW(values[1]);
  if (core == nullptr) {
    std::wcerr << L"MacType preload failed: " << GetLastError() << L'\n';
    return 2;
  }
  const auto safe_unload = reinterpret_cast<LPTHREAD_START_ROUTINE>(
      GetProcAddress(core, "SafeUnload"));
  if (safe_unload == nullptr) {
    std::wcerr << L"MacType SafeUnload export is missing\n";
    return 12;
  }
  if (!WaitForLifecycleStage(diagnostic_namespace, L"hook-ready")) {
    std::wcerr << L"MacType DirectWrite lifecycle did not become ready\n";
    return 7;
  }

  wchar_t proxy_path[32768] = {};
  const DWORD proxy_path_length = GetFullPathNameW(
      values[2], static_cast<DWORD>(std::size(proxy_path)), proxy_path,
      nullptr);
  if (proxy_path_length == 0 || proxy_path_length >= std::size(proxy_path)) {
    return 8;
  }
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto native_load = ntdll == nullptr
                               ? nullptr
                               : reinterpret_cast<NativeLoadLibrary>(
                                     GetProcAddress(ntdll, "LdrLoadDll"));
  if (native_load == nullptr || proxy_path_length > 0x7ffe) {
    return 9;
  }
  NativeUnicodeString proxy_name = {
      static_cast<USHORT>(proxy_path_length * sizeof(wchar_t)),
      static_cast<USHORT>((proxy_path_length + 1) * sizeof(wchar_t)),
      proxy_path,
  };
  HANDLE native_module = nullptr;
  const LONG load_status = native_load(nullptr, nullptr, &proxy_name,
                                       &native_module);
  const HMODULE dwrite_core = static_cast<HMODULE>(native_module);
  if (load_status < 0 || dwrite_core == nullptr) {
    std::wcerr << L"DWriteCore proxy load failed: " << GetLastError() << L'\n';
    return 3;
  }

  const auto raw_address = reinterpret_cast<RawFactoryAddress>(
      GetProcAddress(dwrite_core, "GetRawDWriteCoreCreateFactory"));
  const auto intercepted = reinterpret_cast<CreateFactory>(
      GetProcAddress(dwrite_core, "DWriteCoreCreateFactory"));
  if (raw_address == nullptr || intercepted == nullptr ||
      reinterpret_cast<FARPROC>(intercepted) != raw_address()) {
    std::wcerr << L"DWriteCore lookup leaked a MacType function address\n";
    return 4;
  }
  if (!WaitForLifecycleStage(diagnostic_namespace,
                             L"dwritecore-entry-hooked")) {
    std::wcerr << L"DWriteCore entry hook was not installed synchronously\n";
    return 6;
  }

  IUnknown* factory = nullptr;
  const HRESULT result = intercepted(DWRITE_FACTORY_TYPE_SHARED,
                                     __uuidof(IDWriteFactory), &factory);
  if (FAILED(result) || factory == nullptr) {
    std::wcerr << L"intercepted DWriteCore factory creation failed\n";
    return 5;
  }
  factory->Release();

  if (FreeLibrary(core) == FALSE) {
    std::wcerr << L"the caller's renderer reference could not be released\n";
    return 10;
  }
  HMODULE retained_core = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(core), &retained_core) == FALSE ||
      retained_core != core) {
    std::wcerr << L"plain FreeLibrary unmapped an active renderer\n";
    return 11;
  }

  const HANDLE unload_thread =
      CreateThread(nullptr, 0, safe_unload, nullptr, 0, nullptr);
  if (unload_thread == nullptr ||
      WaitForSingleObject(unload_thread, 10'000) != WAIT_OBJECT_0) {
    if (unload_thread != nullptr) {
      CloseHandle(unload_thread);
    }
    std::wcerr << L"SafeUnload did not reach quiescence\n";
    return 13;
  }
  DWORD unload_status = ERROR_GEN_FAILURE;
  const bool unload_succeeded =
      GetExitCodeThread(unload_thread, &unload_status) != FALSE &&
      unload_status == ERROR_SUCCESS;
  CloseHandle(unload_thread);
  if (!unload_succeeded) {
    std::wcerr << L"SafeUnload failed: " << unload_status << L'\n';
    return 14;
  }
  retained_core = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(core), &retained_core) != FALSE) {
    std::wcerr << L"SafeUnload left the renderer image mapped\n";
    return 15;
  }
  return 0;
}
