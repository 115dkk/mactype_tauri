#pragma once

#include "unity_font_hook_core.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace renderer { namespace unity {

struct SfntFamilyFace final
{
	std::wstring family;
	long faceIndex = 0;
};

[[nodiscard]] std::optional<std::vector<InstalledFontFace>>
EnumerateInstalledFontFaces() noexcept;

[[nodiscard]] std::optional<std::vector<std::wstring>> ParseSfntFamilyNames(
	std::span<const unsigned char> bytes) noexcept;

[[nodiscard]] std::optional<std::vector<SfntFamilyFace>> ParseSfntFamilyFaces(
	std::span<const unsigned char> bytes) noexcept;

}} // namespace renderer::unity
