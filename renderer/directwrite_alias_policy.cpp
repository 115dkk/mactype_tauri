#include "directwrite_alias_policy.h"

#include "bold_face_selection.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
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

bool ContainsName(
	const std::vector<std::wstring>& names,
	const std::wstring& name) noexcept
{
	for (const std::wstring& existing : names)
	{
		if (EqualOrdinalIgnoreCase(existing, name))
			return true;
	}
	return false;
}

// Prefers an upright face, then the weight nearest the regular 400.
bool IsBetterTemplate(
	const SourceFaceObservation& candidate,
	const SourceFaceObservation& current) noexcept
{
	if (candidate.italic != current.italic)
		return !candidate.italic;
	int const candidateDistance = candidate.weight > 400 ?
		candidate.weight - 400 : 400 - candidate.weight;
	int const currentDistance = current.weight > 400 ?
		current.weight - 400 : 400 - current.weight;
	return candidateDistance < currentDistance;
}

constexpr std::size_t kNoFace = static_cast<std::size_t>(-1);

std::size_t SelectTemplateFace(
	const std::vector<SourceFaceObservation>& faces,
	const std::wstring& name) noexcept
{
	std::size_t chosen = kNoFace;
	for (std::size_t index = 0; index < faces.size(); ++index)
	{
		const SourceFaceObservation& face = faces[index];
		if (!ContainsName(face.familyNames, name))
			continue;
		if (renderer::bold_face_selection::IsBoldClassWeight(face.weight))
			return kNoFace;
		if (!face.aliased)
			continue;
		if (chosen == kNoFace)
		{
			chosen = index;
			continue;
		}
		if (!EqualOrdinalIgnoreCase(
				faces[chosen].replacementFamily, face.replacementFamily))
			return kNoFace;
		if (IsBetterTemplate(face, faces[chosen]))
			chosen = index;
	}
	return chosen;
}

} // namespace

bool SharesFamilyName(
	const std::vector<std::wstring>& left,
	const std::vector<std::wstring>& right) noexcept
{
	for (const std::wstring& name : left)
	{
		if (ContainsName(right, name))
			return true;
	}
	return false;
}

bool PlanSyntheticBoldSlots(
	const std::vector<SourceFaceObservation>& faces,
	std::vector<SyntheticBoldSlot>& slots) noexcept
{
	try
	{
		std::vector<SyntheticBoldSlot> planned;
		std::vector<std::wstring> visited;
		for (const SourceFaceObservation& face : faces)
		{
			if (!face.aliased)
				continue;
			for (const std::wstring& name : face.familyNames)
			{
				if (name.empty() || ContainsName(visited, name))
					continue;
				visited.push_back(name);
				std::size_t const templateFace = SelectTemplateFace(faces, name);
				if (templateFace == kNoFace)
					continue;
				auto const slot = std::find_if(
					planned.begin(), planned.end(),
					[templateFace](const SyntheticBoldSlot& existing) {
						return existing.templateFace == templateFace;
					});
				if (slot != planned.end())
				{
					slot->familyNames.push_back(name);
					continue;
				}
				SyntheticBoldSlot added;
				added.familyNames.push_back(name);
				added.templateFace = templateFace;
				planned.push_back(std::move(added));
			}
		}
		slots = std::move(planned);
		return true;
	}
	catch (...)
	{
		slots.clear();
		return false;
	}
}

bool SourceFamilyNames::empty() const noexcept
{
	return win32.empty() && weightStretchStyle.empty() && typographic.empty();
}

std::vector<std::wstring> SourceFamilyNames::All() const
{
	std::vector<std::wstring> names;
	for (auto const* group : {&win32, &weightStretchStyle, &typographic})
	{
		for (auto const& name : *group)
		{
			if (!name.empty() && !ContainsName(names, name))
				names.push_back(name);
		}
	}
	return names;
}

bool ResolveFamilyAliases(
	const SourceFamilyNames& sourceNames,
	const renderer::font_substitution::Snapshot& substitutions,
	FamilyAliasResolution& resolution) noexcept
{
	try
	{
		FamilyAliasResolution candidate;
		std::vector<std::wstring> const* const groups[] = {
			&sourceNames.win32,
			&sourceNames.weightStretchStyle,
			&sourceNames.typographic,
		};
		FallbackAliasResolution matches[3];
		for (std::size_t index = 0; index < 3; ++index)
		{
			auto& match = matches[index];
			for (auto const& name : *groups[index])
			{
				if (name.empty())
					continue;
				auto const resolved = substitutions.Resolve({name, DEFAULT_CHARSET});
				if (resolved.status == renderer::font_substitution::ResolutionStatus::noMatch)
					continue;
				if (resolved.status != renderer::font_substitution::ResolutionStatus::applied ||
					!resolved.matched ||
					(!match.matchedSourceName.empty() &&
						!EqualOrdinalIgnoreCase(match.replacementFamily, resolved.family)))
				{
					resolution = {};
					return false;
				}
				if (match.matchedSourceName.empty())
				{
					match.matchedSourceName = name;
					match.replacementFamily = resolved.family;
				}
			}
			if (candidate.matchedSourceName.empty() && !match.matchedSourceName.empty())
			{
				candidate.matchedSourceName = match.matchedSourceName;
				candidate.replacementFamily = match.replacementFamily;
			}
		}
		if (candidate.matchedSourceName.empty())
		{
			resolution = {};
			return false;
		}
		for (std::size_t index = 0; index < 3; ++index)
		{
			auto const& match = matches[index];
			// Exclude every localized spelling of a differently mapped family.
			// Keeping an unmatched spelling would leak a subfamily into that family.
			if (!match.matchedSourceName.empty() &&
				!EqualOrdinalIgnoreCase(candidate.replacementFamily, match.replacementFamily))
				continue;
			for (auto const& name : *groups[index])
			{
				if (!name.empty() && !ContainsName(candidate.sourceAliases, name))
					candidate.sourceAliases.push_back(name);
			}
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

bool ResolveFallbackAlias(
	const SourceFamilyNames& mappedFontNames,
	const renderer::font_substitution::Snapshot& substitutions,
	FallbackAliasResolution& resolution) noexcept
{
	FamilyAliasResolution family;
	if (!ResolveFamilyAliases(mappedFontNames, substitutions, family))
	{
		resolution = {};
		return false;
	}
	resolution.matchedSourceName = std::move(family.matchedSourceName);
	resolution.replacementFamily = std::move(family.replacementFamily);
	return true;
}

} // namespace directwrite_alias
