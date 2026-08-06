#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace mactype::service_probe::internal {

struct ChildProcessResult {
  DWORD exit_code = ERROR_PROCESS_ABORTED;
  bool launched = false;
};

std::wstring QuoteCommandLineArgument(const std::wstring& argument);

ChildProcessResult LaunchAndWait(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, DWORD timeout_milliseconds);

}  // namespace mactype::service_probe::internal
