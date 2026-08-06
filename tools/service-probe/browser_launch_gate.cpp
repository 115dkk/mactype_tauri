#include "child_process.h"
#include "../../renderer_raii.h"

#include <strsafe.h>

#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD kDefaultTimeoutMilliseconds = 60000;
constexpr DWORD kCleanupWaitMilliseconds = 5000;

std::wstring ReadEnvironmentVariable(const wchar_t* name) {
  const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
  if (length <= 1) {
    return {};
  }
  std::vector<wchar_t> value(length);
  if (GetEnvironmentVariableW(name, value.data(), length) != length - 1) {
    return {};
  }
  return value.data();
}

DWORD ReadTimeoutMilliseconds() {
  const std::wstring value =
      ReadEnvironmentVariable(L"MACTYPE_BROWSER_GATE_TIMEOUT_MS");
  if (value.empty()) {
    return kDefaultTimeoutMilliseconds;
  }
  wchar_t* end = nullptr;
  const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != L'\0' || parsed == 0 ||
      parsed > kDefaultTimeoutMilliseconds) {
    return kDefaultTimeoutMilliseconds;
  }
  return static_cast<DWORD>(parsed);
}

std::wstring SanitizeNamespace(std::wstring value) {
  for (wchar_t& character : value) {
    if (!((character >= L'a' && character <= L'z') ||
          (character >= L'A' && character <= L'Z') ||
          (character >= L'0' && character <= L'9') || character == L'-')) {
      character = L'_';
    }
  }
  return value;
}

bool WriteProcessId(const std::wstring& path, const DWORD process_id) {
  renderer_raii::UniqueHandle file = renderer_raii::AdoptHandle(CreateFileW(
      path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    return false;
  }
  const std::string value = std::to_string(process_id);
  const DWORD value_size = static_cast<DWORD>(value.size());
  DWORD written = 0;
  return WriteFile(file.get(), value.data(), value_size, &written, nullptr) !=
             FALSE &&
         written == value_size && FlushFileBuffers(file.get()) != FALSE;
}

bool WaitForHookEvent(const std::wstring& diagnostic_namespace,
                      const DWORD process_id, const DWORD timeout_milliseconds) {
  wchar_t event_name[256] = {};
  if (FAILED(StringCchPrintfW(
          event_name, ARRAYSIZE(event_name),
          L"Local\\MacType.%s.pid-%lu.hook-entered",
          diagnostic_namespace.c_str(), process_id))) {
    return false;
  }

  const ULONGLONG deadline = GetTickCount64() + timeout_milliseconds;
  do {
    renderer_raii::UniqueHandle event = renderer_raii::AdoptHandle(
        OpenEventW(SYNCHRONIZE, FALSE, event_name));
    if (event && WaitForSingleObject(event.get(), 0) == WAIT_OBJECT_0) {
      return true;
    }
    Sleep(25);
  } while (GetTickCount64() < deadline);
  return false;
}

class SuspendedChild final {
 public:
  explicit SuspendedChild(PROCESS_INFORMATION process) noexcept
      : process_(renderer_raii::AdoptHandle(process.hProcess)),
        thread_(renderer_raii::AdoptHandle(process.hThread)) {}

  SuspendedChild(const SuspendedChild&) = delete;
  SuspendedChild& operator=(const SuspendedChild&) = delete;

  ~SuspendedChild() {
    if (terminate_on_destruction_ && process_) {
      TerminateProcess(process_.get(), ERROR_PROCESS_ABORTED);
      WaitForSingleObject(process_.get(), kCleanupWaitMilliseconds);
    }
  }

  DWORD process_id() const noexcept { return process_id_; }
  void set_process_id(const DWORD value) noexcept { process_id_ = value; }
  HANDLE process_handle() const noexcept { return process_.get(); }
  HANDLE main_thread_handle() const noexcept { return thread_.get(); }

  bool SuspendMainThread() noexcept {
    return thread_ &&
           SuspendThread(thread_.get()) != static_cast<DWORD>(-1);
  }

  bool Resume() noexcept {
    return thread_ && ResumeThread(thread_.get()) != static_cast<DWORD>(-1);
  }

  DWORD Wait() noexcept {
    if (!process_ || WaitForSingleObject(process_.get(), INFINITE) !=
                         WAIT_OBJECT_0) {
      return GetLastError();
    }
    DWORD exit_code = ERROR_PROCESS_ABORTED;
    if (GetExitCodeProcess(process_.get(), &exit_code) == FALSE) {
      exit_code = GetLastError();
    }
    terminate_on_destruction_ = false;
    return exit_code;
  }

 private:
  renderer_raii::UniqueHandle process_;
  renderer_raii::UniqueHandle thread_;
  DWORD process_id_ = 0;
  bool terminate_on_destruction_ = true;
};

class EntryBreakpoint final {
 public:
  EntryBreakpoint() = default;
  EntryBreakpoint(const EntryBreakpoint&) = delete;
  EntryBreakpoint& operator=(const EntryBreakpoint&) = delete;

  ~EntryBreakpoint() { Restore(); }

  bool Arm(HANDLE process, const void* image_base) noexcept {
    IMAGE_DOS_HEADER dos{};
    SIZE_T read = 0;
    if (ReadProcessMemory(process, image_base, &dos, sizeof(dos), &read) ==
            FALSE ||
        read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0) {
      return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(image_base);
    const auto nt_address = base + static_cast<std::uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS headers{};
    if (ReadProcessMemory(process, reinterpret_cast<const void*>(nt_address),
                          &headers, sizeof(headers), &read) == FALSE ||
        read != sizeof(headers) || headers.Signature != IMAGE_NT_SIGNATURE ||
        headers.OptionalHeader.AddressOfEntryPoint == 0U) {
      return false;
    }
    process_ = process;
    address_ = reinterpret_cast<void*>(
        base + headers.OptionalHeader.AddressOfEntryPoint);
    if (ReadProcessMemory(process_, address_, &original_, sizeof(original_),
                          &read) == FALSE ||
        read != sizeof(original_)) {
      return false;
    }
    constexpr BYTE breakpoint = 0xCC;
    if (!WriteByte(breakpoint)) {
      return false;
    }
    armed_ = true;
    return true;
  }

  bool Matches(const EXCEPTION_DEBUG_INFO& exception) const noexcept {
    return armed_ &&
           exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT &&
           exception.ExceptionRecord.ExceptionAddress == address_;
  }

  bool RestoreAndRewind(HANDLE thread) noexcept {
    if (!Restore()) {
      return false;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(thread, &context) == FALSE) {
      return false;
    }
#ifdef _WIN64
    context.Rip = reinterpret_cast<DWORD64>(address_);
#else
    context.Eip = reinterpret_cast<DWORD>(address_);
#endif
    return SetThreadContext(thread, &context) != FALSE;
  }

 private:
  bool WriteByte(const BYTE value) noexcept {
    DWORD old_protection = 0;
    if (VirtualProtectEx(process_, address_, sizeof(value),
                         PAGE_EXECUTE_READWRITE, &old_protection) == FALSE) {
      return false;
    }
    SIZE_T written = 0;
    const bool wrote =
        WriteProcessMemory(process_, address_, &value, sizeof(value),
                           &written) != FALSE &&
        written == sizeof(value) &&
        FlushInstructionCache(process_, address_, sizeof(value)) != FALSE;
    DWORD ignored = 0;
    const bool restored =
        VirtualProtectEx(process_, address_, sizeof(value), old_protection,
                         &ignored) != FALSE;
    return wrote && restored;
  }

  bool Restore() noexcept {
    if (!armed_) {
      return true;
    }
    if (!WriteByte(original_)) {
      return false;
    }
    armed_ = false;
    return true;
  }

  HANDLE process_ = nullptr;
  void* address_ = nullptr;
  BYTE original_ = 0;
  bool armed_ = false;
};

void CloseDebugEventFile(const DEBUG_EVENT& event) noexcept {
  HANDLE file = nullptr;
  if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
    file = event.u.CreateProcessInfo.hFile;
  } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
    file = event.u.LoadDll.hFile;
  }
  if (file != nullptr) {
    CloseHandle(file);
  }
}

bool StopAtImageEntryPoint(SuspendedChild& child,
                           const DWORD timeout_milliseconds) {
  const ULONGLONG deadline = GetTickCount64() + timeout_milliseconds;
  EntryBreakpoint entry_breakpoint;
  bool initial_breakpoint_seen = false;
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      SetLastError(ERROR_TIMEOUT);
      return false;
    }
    const auto remaining = static_cast<DWORD>(deadline - now);
    DEBUG_EVENT event{};
    if (WaitForDebugEvent(&event, remaining) == FALSE) {
      return false;
    }
    if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT &&
        !entry_breakpoint.Arm(child.process_handle(),
                              event.u.CreateProcessInfo.lpBaseOfImage)) {
      CloseDebugEventFile(event);
      ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
      return false;
    }
    CloseDebugEventFile(event);

    if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
        entry_breakpoint.Matches(event.u.Exception)) {
      if (!entry_breakpoint.RestoreAndRewind(child.main_thread_handle()) ||
          !child.SuspendMainThread()) {
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId,
                           DBG_EXCEPTION_NOT_HANDLED);
        return false;
      }
      if (ContinueDebugEvent(event.dwProcessId, event.dwThreadId,
                             DBG_CONTINUE) == FALSE) {
        return false;
      }
      return DebugActiveProcessStop(child.process_id()) != FALSE;
    }

    if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
        event.u.Exception.dwFirstChance != FALSE &&
        event.u.Exception.ExceptionRecord.ExceptionCode ==
            EXCEPTION_BREAKPOINT &&
        !initial_breakpoint_seen) {
      initial_breakpoint_seen = true;
      if (ContinueDebugEvent(event.dwProcessId, event.dwThreadId,
                             DBG_CONTINUE) == FALSE) {
        return false;
      }
      continue;
    }

    if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
      ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
      SetLastError(ERROR_PROCESS_ABORTED);
      return false;
    }
    const DWORD continue_status =
        event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT
            ? DBG_EXCEPTION_NOT_HANDLED
            : DBG_CONTINUE;
    if (ContinueDebugEvent(event.dwProcessId, event.dwThreadId,
                           continue_status) == FALSE) {
      return false;
    }
  }
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") {
    return 0;
  }

  const std::wstring target =
      ReadEnvironmentVariable(L"MACTYPE_BROWSER_GATE_TARGET");
  const std::wstring pid_file =
      ReadEnvironmentVariable(L"MACTYPE_BROWSER_GATE_PID_FILE");
  const std::wstring diagnostic_namespace = SanitizeNamespace(
      ReadEnvironmentVariable(L"MACTYPE_DIRECTWRITE_DIAGNOSTICS"));
  const DWORD timeout_milliseconds = ReadTimeoutMilliseconds();
  if (target.empty() || pid_file.empty() || diagnostic_namespace.empty()) {
    return ERROR_INVALID_PARAMETER;
  }

  SetEnvironmentVariableW(L"MACTYPE_BROWSER_GATE_TARGET", nullptr);
  SetEnvironmentVariableW(L"MACTYPE_BROWSER_GATE_PID_FILE", nullptr);
  SetEnvironmentVariableW(L"MACTYPE_BROWSER_GATE_TIMEOUT_MS", nullptr);

  std::wstring command =
      mactype::service_probe::internal::QuoteCommandLineArgument(target);
  for (int index = 1; index < argc; ++index) {
    command.push_back(L' ');
    command +=
        mactype::service_probe::internal::QuoteCommandLineArgument(argv[index]);
  }
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  if (CreateProcessW(target.c_str(), mutable_command.data(), nullptr, nullptr,
                     TRUE, DEBUG_ONLY_THIS_PROCESS | CREATE_UNICODE_ENVIRONMENT,
                     nullptr, nullptr, &startup, &process) == FALSE) {
    return static_cast<int>(GetLastError());
  }

  SuspendedChild child(process);
  child.set_process_id(process.dwProcessId);
  if (!StopAtImageEntryPoint(child, timeout_milliseconds)) {
    return static_cast<int>(GetLastError());
  }
  if (!WriteProcessId(pid_file, child.process_id())) {
    return static_cast<int>(GetLastError());
  }
  if (!WaitForHookEvent(diagnostic_namespace, child.process_id(),
                        timeout_milliseconds)) {
    return ERROR_TIMEOUT;
  }
  if (!child.Resume()) {
    return static_cast<int>(GetLastError());
  }
  return static_cast<int>(child.Wait());
}
