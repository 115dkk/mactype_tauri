#pragma once

#include "unity_font_hook_core.h"

#include <cstddef>
#include <string>
#include <vector>

namespace renderer { namespace unity {

bool EnumerateInstalledFontFaces(
	std::vector<InstalledFontFace>& fonts) noexcept;

bool ParseSfntFamilyNames(
	const unsigned char* bytes,
	std::size_t size,
	std::vector<std::wstring>& familyNames) noexcept;

}} // namespace renderer::unity
