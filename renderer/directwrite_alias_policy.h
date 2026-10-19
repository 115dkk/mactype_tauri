#pragma once

#include "font_substitution.h"

#include <cstddef>
#include <string>
#include <vector>

namespace directwrite_alias {

// Localized names remain grouped by family model. A Win32 subfamily rule
// takes precedence over a broader WWS or typographic family rule.
struct SourceFamilyNames final
{
	std::vector<std::wstring> win32;
	std::vector<std::wstring> weightStretchStyle;
	std::vector<std::wstring> typographic;

	bool empty() const noexcept;
	std::vector<std::wstring> All() const;
};

struct FamilyAliasResolution final
{
	std::wstring matchedSourceName;
	std::wstring replacementFamily;
	std::vector<std::wstring> sourceAliases;
};

struct FallbackAliasResolution final
{
	std::wstring matchedSourceName;
	std::wstring replacementFamily;
};

bool ResolveFamilyAliases(
	const SourceFamilyNames& sourceNames,
	const renderer::font_substitution::Snapshot& substitutions,
	FamilyAliasResolution& resolution) noexcept;

bool ResolveFallbackAlias(
	const SourceFamilyNames& mappedFontNames,
	const renderer::font_substitution::Snapshot& substitutions,
	FallbackAliasResolution& resolution) noexcept;

// One source face as the alias build saw it. `aliased` is false for a face
// kept native, whether no rule resolved it or its replacement failed.
struct SourceFaceObservation final
{
	std::vector<std::wstring> familyNames;
	int weight = 400;
	bool italic = false;
	bool aliased = false;
	std::wstring replacementFamily;
};

struct SyntheticBoldSlot final
{
	std::vector<std::wstring> familyNames;
	// Index of the aliased regular-class observation that lends the slot its
	// stretch, style and replacement family.
	std::size_t templateFace = 0;
};

// A family name gets a synthesized bold slot when an aliased face carries it
// and no observed face carrying it, aliased or native, is bold-class. Names
// whose aliased faces disagree on the replacement family get no slot. Each
// name appears in at most one slot; names sharing a template face share a slot.
bool PlanSyntheticBoldSlots(
	const std::vector<SourceFaceObservation>& faces,
	std::vector<SyntheticBoldSlot>& slots) noexcept;

bool SharesFamilyName(
	const std::vector<std::wstring>& left,
	const std::vector<std::wstring>& right) noexcept;

} // namespace directwrite_alias
