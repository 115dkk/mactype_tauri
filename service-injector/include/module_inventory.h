#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string_view>

namespace mactype::injector {

enum class FixedModuleState {
    Absent,
    ExpectedModuleLoaded,
    SameBasenameDifferentPath,
    InventoryUnavailable,
};

[[nodiscard]] bool module_paths_equal(std::wstring_view left,
                                      std::wstring_view right) noexcept;
[[nodiscard]] FixedModuleState fixed_module_state(
    HANDLE process, const std::filesystem::path& expected_path) noexcept;
[[nodiscard]] std::optional<LPTHREAD_START_ROUTINE> remote_load_library(
    HANDLE process) noexcept;

}  // namespace mactype::injector
