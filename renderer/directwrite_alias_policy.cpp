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
