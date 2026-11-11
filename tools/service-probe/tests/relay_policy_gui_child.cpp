#include <windows.h>
#include <shellapi.h>

#include <string_view>

// A GUI-subsystem child for relay-policy-probe: it reports whether the named
// module was mapped before its entry point ran.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** const argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr || argc != 4 || std::wstring_view(argv[1]) != L"--module") {
    return 64;
  }
  const bool expect_present = std::wstring_view(argv[3]) == L"--expect-present";
  const bool expect_absent = std::wstring_view(argv[3]) == L"--expect-absent";
  const bool present = GetModuleHandleW(argv[2]) != nullptr;
  LocalFree(argv);
  if (!expect_present && !expect_absent) {
    return 64;
  }
  return present == expect_present ? 0 : 3;
}
