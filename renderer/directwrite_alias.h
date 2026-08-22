#pragma once

#include "common.h"

#include <cstdint>

namespace directwrite_alias {

enum class BuildStatus
{
	applied,
	appliedWithMissingReplacement,
	noSubstitutions,
	settingsNotInitialized,
	noResolvedSubstitutions,
	unsupportedFactory,
	systemSetUnavailable,
	builderUnavailable,
	replacementUnavailable,
	virtualFontFailed,
	aliasReferenceRejected,
	outOfMemory,
	unexpectedFailure,
	addFontFailed,
	createSetFailed,
	createCollectionFailed,
};

struct AliasFontSet
{
	CComPtr<IDWriteFontSet> fontSet;
	CComPtr<IDWriteFontCollection1> collection;
	UINT32 substitutionCount = 0;
	std::uint64_t substitutionGeneration = 0;
	std::uint64_t substitutionDigest = 0;
	BuildStatus status = BuildStatus::noSubstitutions;
};

BuildStatus GetOrCreate(
	IDWriteFactory* factory,
	IDWriteFontSet* systemFontSet,
	AliasFontSet& result) noexcept;

void ClearCache() noexcept;

WCHAR const* StatusName(BuildStatus status) noexcept;

} // namespace directwrite_alias
