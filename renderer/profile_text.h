#pragma once

#include <cstddef>
#include <string>

namespace renderer {
namespace profile_text {

// Decodes profile bytes in the order the Control Center reads them: a UTF-16
// or UTF-8 byte-order mark, then strict UTF-8, then the legacy code page.
// The legacy code page is the system ANSI code page, never CP_ACP: a process
// whose manifest declares activeCodePage UTF-8 reports 65001 for CP_ACP and
// would turn every legacy non-ASCII family name into replacement characters.
// When the legacy code page rejects the bytes, the result is the same lossy
// decode the renderer used before, so no profile that loaded before is refused.
// Returns false only for empty input or an allocation failure.
bool Decode(
	const unsigned char* data,
	std::size_t size,
	unsigned int legacyCodePage,
	std::wstring& text);

// The system default ANSI code page, independent of the process manifest.
// Falls back to CP_ACP when the system locale cannot be queried.
unsigned int SystemAnsiCodePage() noexcept;

} // namespace profile_text
} // namespace renderer
