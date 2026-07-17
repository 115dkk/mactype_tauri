#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string_view>

namespace mactype::injector {

[[nodiscard]] bool module_paths_equal(std::wstring_view left,
                                      std::wstring_view right) noexcept;
[[nodiscard]] std::optional<bool> fixed_module_is_loaded(
    HANDLE process, const std::filesystem::path& expected_path) noexcept;
[[nodiscard]] std::optional<LPTHREAD_START_ROUTINE> remote_load_library(
    HANDLE process) noexcept;

}  // namespace mactype::injector
