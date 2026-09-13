#pragma once

#include <windows.h>

namespace renderer {

// Invalidates every visible top-level window owned by processId, children and
// frame included, without waiting for the repaint. Returns the number of
// windows invalidated. Safe to call from any thread of the process.
unsigned int RepaintOwnedWindows(DWORD processId) noexcept;

} // namespace renderer
