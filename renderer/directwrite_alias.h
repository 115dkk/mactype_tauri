#pragma once

#include "common.h"

#include <chrono>
#include <cstdint>
#include <memory>

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
	buildReentrant,
	waitTimeout,
};

struct AliasFontSet
{
	CComPtr<IDWriteFontSet> fontSet;
	CComPtr<IDWriteFontSet> downloadableFontSet;
	CComPtr<IDWriteFontCollection1> collection;
	CComPtr<IDWriteFontCollection1> downloadableCollection;
	UINT32 substitutionCount = 0;
	std::uint64_t substitutionGeneration = 0;
	std::uint64_t substitutionDigest = 0;
	std::uint64_t buildMicroseconds = 0;
	BuildStatus status = BuildStatus::noSubstitutions;
	bool cacheHit = false;
};

// Reserves the per-factory build entry before the fallback slots are exposed,
// so a fallback lookup that arrives before the build starts finds an entry to
// wait on instead of the native font. The reserving thread's own GetOrCreate
// adopts the entry and builds it; a reservation destroyed unconsumed (an early
// return in the preamble) completes the entry as unexpectedFailure and wakes
// the waiters.
class BuildReservation
{
public:
	BuildReservation(
		IDWriteFactory* factory,
		IDWriteFontSet* systemFontSet) noexcept;
	~BuildReservation();
	BuildReservation(BuildReservation const&) = delete;
	BuildReservation& operator=(BuildReservation const&) = delete;
	bool reserved() const noexcept;

private:
	std::shared_ptr<void> entry_;
};

BuildStatus GetOrCreate(
	IDWriteFactory* factory,
	IDWriteFontSet* systemFontSet,
	AliasFontSet& result) noexcept;

// True when collection is one of the alias collections this process built.
bool IsAliasCollection(IUnknown* collection) noexcept;

BuildStatus GetCachedForFactory(
	IDWriteFactory* factory,
	std::uint64_t substitutionGeneration,
	AliasFontSet& result) noexcept;

std::chrono::milliseconds BuildWaitTimeout() noexcept;

BuildStatus GetCachedForFactory(
	IDWriteFactory* factory,
	std::uint64_t substitutionGeneration,
	AliasFontSet& result,
	std::chrono::steady_clock::time_point deadline) noexcept;

void ClearCache() noexcept;

WCHAR const* StatusName(BuildStatus status) noexcept;

} // namespace directwrite_alias
