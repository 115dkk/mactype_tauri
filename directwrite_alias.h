#pragma once

#include "common.h"

namespace directwrite_alias {

enum class BuildStatus
{
	applied,
	appliedWithMissingReplacement,
	noSubstitutions,
	unsupportedFactory,
	systemSetUnavailable,
	builderUnavailable,
	addFontFailed,
	createSetFailed,
	createCollectionFailed,
};

struct AliasFontSet
{
	CComPtr<IDWriteFontSet> fontSet;
	CComPtr<IDWriteFontCollection1> collection;
	UINT32 substitutionCount = 0;
	BuildStatus status = BuildStatus::noSubstitutions;
};

BuildStatus GetOrCreate(
	IDWriteFactory* factory,
	IDWriteFontSet* systemFontSet,
	AliasFontSet& result);

void ClearCache() noexcept;

WCHAR const* StatusName(BuildStatus status) noexcept;

} // namespace directwrite_alias
