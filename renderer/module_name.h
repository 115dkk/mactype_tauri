#pragma once

#include <windows.h>

namespace renderer {
namespace module_name {

// Case-insensitive comparison of a loaded module's base file name. Loader
// hooks call this on whichever thread loads a module, including driver and
// engine worker threads with 64 KB stacks, so the lookup never places a
// maximum-length path on the caller's stack.
[[nodiscard]] bool BaseNameEquals(
    HMODULE module,
    const wchar_t* expectedBaseName) noexcept;

} // namespace module_name
} // namespace renderer
