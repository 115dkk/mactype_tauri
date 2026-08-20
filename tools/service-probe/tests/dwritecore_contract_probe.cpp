#include <windows.h>
#include <dwrite.h>

#include <iostream>
#include <string>

namespace {

using CreateFactory = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
using RawFactoryAddress = FARPROC(WINAPI*)();

bool WaitForLifecycleStage(const std::wstring& diagnostic_namespace,
                           const wchar_t* stage) {
  const std::wstring event_name =
      L"Local\\MacType." + diagnostic_namespace + L".pid-" +
      std::to_wstring(GetCurrentProcessId()) + L"." + stage;
  const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, event_name.c_str());
  if (event == nullptr) {
    return false;
  }
  const DWORD wait = WaitForSingleObject(event, 3000);
  CloseHandle(event);
  return wait == WAIT_OBJECT_0;
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
  const HMODULE dwrite_core = LoadLibraryW(values[2]);
  if (dwrite_core == nullptr) {
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
  return 0;
}
