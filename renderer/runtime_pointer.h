#pragma once

#include <string>

namespace renderer::runtime_pointer {

bool CurrentRuntimeDirectory(
    std::wstring const& moduleDirectory,
    std::wstring& runtimeDirectory) noexcept;

} // namespace renderer::runtime_pointer
