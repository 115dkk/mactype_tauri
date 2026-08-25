#pragma once

#include "font_substitution.h"

#include <string>
#include <vector>

namespace directwrite_alias {

struct FamilyAliasResolution final
{
	std::wstring replacementFamily;
	std::vector<std::wstring> sourceAliases;
};

bool ResolveFamilyAliases(
	const std::vector<std::wstring>& sourceAliases,
	const renderer::font_substitution::Snapshot& substitutions,
	FamilyAliasResolution& resolution) noexcept;

} // namespace directwrite_alias
