#include "directwrite_alias_policy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <limits>

namespace directwrite_alias {
namespace {

bool EqualOrdinalIgnoreCase(
	const std::wstring& left,
	const std::wstring& right) noexcept
{
	if (left.size() != right.size() ||
		left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return false;
	return CompareStringOrdinal(
		left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

bool ResolveFamilyAliases(
	const std::vector<std::wstring>& sourceAliases,
	const renderer::font_substitution::Snapshot& substitutions,
	FamilyAliasResolution& resolution) noexcept
{
	try
	{
		FamilyAliasResolution candidate;
		for (const std::wstring& alias : sourceAliases)
		{
			if (alias.empty())
				continue;
			bool duplicate = false;
			for (const std::wstring& existing : candidate.sourceAliases)
			{
				if (EqualOrdinalIgnoreCase(existing, alias))
				{
					duplicate = true;
					break;
				}
			}
			if (!duplicate)
				candidate.sourceAliases.push_back(alias);
		}

		bool matched = false;
		for (const std::wstring& alias : candidate.sourceAliases)
		{
			renderer::font_substitution::Resolution const resolved =
				substitutions.Resolve({alias, DEFAULT_CHARSET});
			if (resolved.status ==
				renderer::font_substitution::ResolutionStatus::noMatch)
				continue;
			if (resolved.status !=
				renderer::font_substitution::ResolutionStatus::applied ||
				!resolved.matched)
			{
				resolution = {};
				return false;
			}
			if (!matched)
			{
				candidate.replacementFamily = resolved.family;
				matched = true;
			}
			else if (!EqualOrdinalIgnoreCase(
				candidate.replacementFamily, resolved.family))
			{
				resolution = {};
				return false;
			}
		}
		if (!matched || candidate.sourceAliases.empty())
		{
			resolution = {};
			return false;
		}
		resolution = std::move(candidate);
		return true;
	}
	catch (...)
	{
		resolution = {};
		return false;
	}
}

} // namespace directwrite_alias
