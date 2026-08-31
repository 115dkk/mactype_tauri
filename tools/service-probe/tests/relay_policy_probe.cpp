#include <windows.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kExplicitPrivateFreeTypeMarker[] =
    L"windows:fontengine=freetype";

struct HandleCloser {
  void operator()(HANDLE handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

class AttributeList final {
 public:
  bool Initialize(DWORD64 policy) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (bytes == 0) {
      return false;
    }
    storage_.resize((bytes + sizeof(ULONG_PTR) - 1) / sizeof(ULONG_PTR));
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (InitializeProcThreadAttributeList(list_, 1, 0, &bytes) == FALSE) {
      list_ = nullptr;
      return false;
    }
    initialized_ = true;
    return UpdateProcThreadAttribute(
               list_, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
               &policy, sizeof(policy), nullptr, nullptr) != FALSE;
  }

  ~AttributeList() {
    if (initialized_) {
      DeleteProcThreadAttributeList(list_);
    }
  }

  LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

 private:
  std::vector<ULONG_PTR> storage_;
  LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
  bool initialized_ = false;
};

std::wstring Quote(const std::wstring& value) {
  std::wstring result = L"\"";
  for (const wchar_t character : value) {
    if (character == L'\"') {
      result.push_back(L'\\');
    }
    result.push_back(character);
  }
  result.push_back(L'\"');
  return result;
}

bool LaunchChild(const std::filesystem::path& self,
                 const std::wstring& module_name, const bool prohibit_dynamic,
                 const bool expect_present) {
  std::wstring command = Quote(self.wstring()) + L" --child --module " +
                         Quote(module_name) +
                         (expect_present ? L" --expect-present"
                                         : L" --expect-absent");
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOEXW extended{};
  extended.StartupInfo.cb = sizeof(extended);
  STARTUPINFOW ordinary{};
  ordinary.cb = sizeof(ordinary);
  AttributeList attributes;
  DWORD flags = CREATE_UNICODE_ENVIRONMENT;
  LPSTARTUPINFOW startup = &ordinary;
  if (prohibit_dynamic) {
    if (!attributes.Initialize(
            PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON)) {
      return false;
    }
    extended.lpAttributeList = attributes.get();
    flags |= EXTENDED_STARTUPINFO_PRESENT;
    startup = &extended.StartupInfo;
  }

  PROCESS_INFORMATION process{};
  if (CreateProcessW(self.c_str(), mutable_command.data(), nullptr, nullptr,
                     FALSE, flags, nullptr, nullptr, startup, &process) ==
      FALSE) {
    return false;
  }
  const UniqueHandle process_handle(process.hProcess);
  const UniqueHandle thread_handle(process.hThread);
  if (WaitForSingleObject(process_handle.get(), 15'000) != WAIT_OBJECT_0) {
    TerminateProcess(process_handle.get(), ERROR_TIMEOUT);
    return false;
  }
  DWORD exit_code = ERROR_GEN_FAILURE;
  return GetExitCodeProcess(process_handle.get(), &exit_code) != FALSE &&
         exit_code == ERROR_SUCCESS;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
  if (argc == 5 && std::wstring_view(argv[1]) == L"--child" &&
      std::wstring_view(argv[2]) == L"--module") {
    const bool expect_present =
        std::wstring_view(argv[4]) == L"--expect-present";
    const bool expect_absent =
        std::wstring_view(argv[4]) == L"--expect-absent";
    if (!expect_present && !expect_absent) {
      return 64;
    }
    const bool present = GetModuleHandleW(argv[3]) != nullptr;
    return present == expect_present ? 0 : 3;
  }
  const bool expect_private_freetype_skip =
      argc == 3 &&
      std::wstring_view(argv[2]) == L"--expect-private-freetype-skip";
  if (argc != 2 && !expect_private_freetype_skip) {
    std::wcerr << L"Usage: relay-policy-probe{32|64}.exe <MacType DLL>\n";
    return 64;
  }
  GetEnvironmentVariableW(kExplicitPrivateFreeTypeMarker, nullptr, 0);

  const std::filesystem::path core =
      std::filesystem::absolute(std::filesystem::path(argv[1]));
  const HMODULE loaded_core = LoadLibraryW(core.c_str());
  if (loaded_core == nullptr) {
    return 2;
  }
  const std::filesystem::path self =
      std::filesystem::path([&] {
        std::vector<wchar_t> path(32'768, L'\0');
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= static_cast<DWORD>(path.size())) {
          return std::wstring{};
        }
        return std::wstring(path.data(), length);
      }());
  if (self.empty()) {
    return 2;
  }
  const std::wstring module_name = core.filename().wstring();

  if (!LaunchChild(self, module_name, true, false)) {
    return 4;
  }
  if (!LaunchChild(
          self, module_name, false, !expect_private_freetype_skip)) {
    return 5;
  }
  return 0;
}
