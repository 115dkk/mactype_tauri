#pragma once

#include "unity_font_hook_core.h"

#include <cstddef>
#include <string>
#include <vector>

namespace renderer { namespace unity {

struct SfntFamilyFace final
{
	std::wstring family;
	long faceIndex = 0;
};

bool EnumerateInstalledFontFaces(
	std::vector<InstalledFontFace>& fonts) noexcept;

bool ParseSfntFamilyNames(
	const unsigned char* bytes,
	std::size_t size,
	std::vector<std::wstring>& familyNames) noexcept;

bool ParseSfntFamilyFaces(
	const unsigned char* bytes,
	std::size_t size,
	std::vector<SfntFamilyFace>& familyFaces) noexcept;

}} // namespace renderer::unity
