#include "unity_font_evidence.h"

#include <strsafe.h>

#include <cstring>
#include <limits>

namespace renderer { namespace unity {
namespace {

bool TryAcquireEvidenceLock(UnityFontEvidenceV1& evidence) noexcept
{
	for (unsigned int spin = 0; spin < 4096; ++spin)
	{
		if (InterlockedCompareExchange(&evidence.writerLock, 1, 0) == 0)
			return true;
		YieldProcessor();
	}
	return false;
}

void UnlockEvidence(UnityFontEvidenceV1& evidence) noexcept
{
	MemoryBarrier();
	InterlockedExchange(&evidence.writerLock, 0);
}

} // namespace

void InitializeUnityFontEvidence(
	UnityFontEvidenceV1& evidence,
	DWORD pid) noexcept
{
	std::memset(&evidence, 0, sizeof(evidence));
	evidence.structSize = sizeof(evidence);
	evidence.schema = kUnityFontEvidenceSchema;
	evidence.pid = pid;
}

void RecordUnityFontRedirect(
	UnityFontEvidenceV1& evidence,
	const wchar_t* sourcePath,
	const wchar_t* replacementPath,
	bool succeeded) noexcept
{
	if (evidence.structSize != sizeof(evidence) ||
		evidence.schema != kUnityFontEvidenceSchema)
		return;
	if (TryAcquireEvidenceLock(evidence))
	{
		if (evidence.redirectAttempts != (std::numeric_limits<LONG>::max)())
			++evidence.redirectAttempts;
		LONG& outcome = succeeded
			? evidence.redirectSuccesses
			: evidence.redirectFallbacks;
		if (outcome != (std::numeric_limits<LONG>::max)())
			++outcome;
		StringCchCopyW(
			evidence.sourcePath, kUnityFontEvidencePathChars,
			sourcePath != nullptr ? sourcePath : L"");
		StringCchCopyW(
			evidence.replacementPath, kUnityFontEvidencePathChars,
			replacementPath != nullptr ? replacementPath : L"");
		UnlockEvidence(evidence);
	}
}

void RecordUnityFontFileOpen(
	UnityFontEvidenceV1& evidence,
	const wchar_t* path) noexcept
{
	if (evidence.structSize != sizeof(evidence) ||
		evidence.schema != kUnityFontEvidenceSchema || path == nullptr ||
		!TryAcquireEvidenceLock(evidence))
		return;
	if (evidence.observedFontOpens != (std::numeric_limits<LONG>::max)())
		++evidence.observedFontOpens;
	StringCchCopyW(
		evidence.observedPath, kUnityFontEvidencePathChars, path);
	UnlockEvidence(evidence);
}

void RecordUnityFontRender(
	UnityFontEvidenceV1& evidence,
	LONG error,
	unsigned int glyphIndex,
	unsigned int bitmapWidth,
	unsigned int bitmapRows) noexcept
{
	if (evidence.structSize != sizeof(evidence) ||
		evidence.schema != kUnityFontEvidenceSchema ||
		!TryAcquireEvidenceLock(evidence))
		return;
	if (evidence.renderCalls != (std::numeric_limits<LONG>::max)())
		++evidence.renderCalls;
	if (error == 0 &&
		evidence.renderSuccesses != (std::numeric_limits<LONG>::max)())
		++evidence.renderSuccesses;
	if (error == 0 && bitmapWidth != 0 && bitmapRows != 0 &&
		evidence.nonEmptyBitmaps != (std::numeric_limits<LONG>::max)())
		++evidence.nonEmptyBitmaps;
	evidence.lastRenderError = error;
	auto const bounded = [](unsigned int value) noexcept {
		return value > static_cast<unsigned int>((std::numeric_limits<LONG>::max)())
			? (std::numeric_limits<LONG>::max)()
			: static_cast<LONG>(value);
	};
	evidence.lastGlyphIndex = bounded(glyphIndex);
	evidence.lastBitmapWidth = bounded(bitmapWidth);
	evidence.lastBitmapRows = bounded(bitmapRows);
	UnlockEvidence(evidence);
}

void RecordUnityFontFaceDetails(
	UnityFontEvidenceV1& evidence,
	bool hasCharmap,
	LONG faceGlyphs,
	unsigned int sampleKoreanGlyph) noexcept
{
	if (evidence.structSize != sizeof(evidence) ||
		evidence.schema != kUnityFontEvidenceSchema ||
		!TryAcquireEvidenceLock(evidence))
		return;
	evidence.redirectedFaceHasCharmap = hasCharmap ? 1 : 0;
	evidence.redirectedFaceGlyphs = faceGlyphs;
	evidence.sampleKoreanGlyph = sampleKoreanGlyph >
			static_cast<unsigned int>((std::numeric_limits<LONG>::max)())
		? (std::numeric_limits<LONG>::max)()
		: static_cast<LONG>(sampleKoreanGlyph);
	UnlockEvidence(evidence);
}

void RecordUnityFontCharacterLookup(
	UnityFontEvidenceV1& evidence,
	const wchar_t* family,
	bool mapped,
	unsigned int character,
	bool found,
	unsigned int glyphIndex,
	unsigned int sampleKoreanGlyph,
	const wchar_t* resolvedFaceFamily) noexcept
{
	if (evidence.structSize != sizeof(evidence) ||
		evidence.schema != kUnityFontEvidenceSchema ||
		!TryAcquireEvidenceLock(evidence))
		return;
	if (evidence.characterLookups != (std::numeric_limits<LONG>::max)())
		++evidence.characterLookups;
	if (found &&
		evidence.characterLookupHits != (std::numeric_limits<LONG>::max)())
		++evidence.characterLookupHits;
	auto const bounded = [](unsigned int value) noexcept {
		return value > static_cast<unsigned int>((std::numeric_limits<LONG>::max)())
			? (std::numeric_limits<LONG>::max)()
			: static_cast<LONG>(value);
	};
	evidence.lastCharacter = bounded(character);
	evidence.lastLookupGlyph = bounded(glyphIndex);
	StringCchCopyW(
		evidence.lastLookupFamily, kUnityFontEvidenceFamilyChars,
		family != nullptr ? family : L"");
	if (mapped)
	{
		if (evidence.mappedCharacterLookups !=
				(std::numeric_limits<LONG>::max)())
			++evidence.mappedCharacterLookups;
		if (found && evidence.mappedCharacterLookupHits !=
				(std::numeric_limits<LONG>::max)())
			++evidence.mappedCharacterLookupHits;
		evidence.lastMappedCharacter = bounded(character);
		evidence.lastMappedGlyph = bounded(glyphIndex);
		evidence.lastMappedSampleKoreanGlyph = bounded(sampleKoreanGlyph);
		StringCchCopyW(
			evidence.lastMappedFamily, kUnityFontEvidenceFamilyChars,
			family != nullptr ? family : L"");
		StringCchCopyW(
			evidence.lastMappedResolvedFaceFamily,
			kUnityFontEvidenceFamilyChars,
			resolvedFaceFamily != nullptr ? resolvedFaceFamily : L"");
	}
	UnlockEvidence(evidence);
}

void RecordUnityFontFaceResolution(
	UnityFontEvidenceV1& evidence,
	bool found,
	unsigned int glyphIndex,
	unsigned int sampleKoreanGlyph) noexcept
{
	if (evidence.structSize != sizeof(evidence) ||
		evidence.schema != kUnityFontEvidenceSchema ||
		!TryAcquireEvidenceLock(evidence))
		return;
	if (evidence.faceResolutions != (std::numeric_limits<LONG>::max)())
		++evidence.faceResolutions;
	if (found &&
		evidence.faceResolutionHits != (std::numeric_limits<LONG>::max)())
		++evidence.faceResolutionHits;
	if (glyphIndex != 0 &&
		evidence.faceResolutionGlyphHits != (std::numeric_limits<LONG>::max)())
		++evidence.faceResolutionGlyphHits;
	evidence.lastFaceResolutionGlyph = glyphIndex >
			static_cast<unsigned int>((std::numeric_limits<LONG>::max)())
		? (std::numeric_limits<LONG>::max)()
		: static_cast<LONG>(glyphIndex);
	evidence.lastFaceResolutionSampleKoreanGlyph = sampleKoreanGlyph >
			static_cast<unsigned int>((std::numeric_limits<LONG>::max)())
		? (std::numeric_limits<LONG>::max)()
		: static_cast<LONG>(sampleKoreanGlyph);
	UnlockEvidence(evidence);
}

void RecordUnityFontOsFaceResolution(
	UnityFontEvidenceV1& evidence,
	const wchar_t* family,
	bool mapped,
	bool found,
	unsigned int sampleKoreanGlyph,
	const wchar_t* resolvedFaceFamily) noexcept
{
	if (evidence.structSize != sizeof(evidence) ||
		evidence.schema != kUnityFontEvidenceSchema ||
		!TryAcquireEvidenceLock(evidence))
		return;
	if (evidence.osFaceResolutions != (std::numeric_limits<LONG>::max)())
		++evidence.osFaceResolutions;
	if (found &&
		evidence.osFaceResolutionHits != (std::numeric_limits<LONG>::max)())
		++evidence.osFaceResolutionHits;
	evidence.lastOsFaceSampleKoreanGlyph = sampleKoreanGlyph >
			static_cast<unsigned int>((std::numeric_limits<LONG>::max)())
		? (std::numeric_limits<LONG>::max)()
		: static_cast<LONG>(sampleKoreanGlyph);
	StringCchCopyW(
		evidence.lastOsFaceFamily, kUnityFontEvidenceFamilyChars,
		family != nullptr ? family : L"");
	StringCchCopyW(
		evidence.lastOsResolvedFaceFamily, kUnityFontEvidenceFamilyChars,
		resolvedFaceFamily != nullptr ? resolvedFaceFamily : L"");
	if (mapped)
	{
		if (evidence.mappedOsFaceResolutions !=
				(std::numeric_limits<LONG>::max)())
			++evidence.mappedOsFaceResolutions;
		if (found && evidence.mappedOsFaceResolutionHits !=
				(std::numeric_limits<LONG>::max)())
			++evidence.mappedOsFaceResolutionHits;
		evidence.lastMappedOsFaceSampleKoreanGlyph =
			evidence.lastOsFaceSampleKoreanGlyph;
		StringCchCopyW(
			evidence.lastMappedOsFaceFamily, kUnityFontEvidenceFamilyChars,
			family != nullptr ? family : L"");
		StringCchCopyW(
			evidence.lastMappedOsResolvedFaceFamily,
			kUnityFontEvidenceFamilyChars,
			resolvedFaceFamily != nullptr ? resolvedFaceFamily : L"");
	}
	UnlockEvidence(evidence);
}

bool ReadUnityFontEvidence(
	const UnityFontEvidenceV1& evidence,
	UnityFontEvidenceSnapshot& snapshot) noexcept
{
	snapshot = {};
	if (evidence.structSize != sizeof(evidence) ||
		evidence.schema != kUnityFontEvidenceSchema)
		return false;
	UnityFontEvidenceV1& mutableEvidence =
		const_cast<UnityFontEvidenceV1&>(evidence);
	if (!TryAcquireEvidenceLock(mutableEvidence))
		return false;
	MemoryBarrier();
	snapshot.pid = evidence.pid;
	snapshot.redirectAttempts = evidence.redirectAttempts;
	snapshot.redirectSuccesses = evidence.redirectSuccesses;
	snapshot.redirectFallbacks = evidence.redirectFallbacks;
	snapshot.observedFontOpens = evidence.observedFontOpens;
	snapshot.renderCalls = evidence.renderCalls;
	snapshot.renderSuccesses = evidence.renderSuccesses;
	snapshot.nonEmptyBitmaps = evidence.nonEmptyBitmaps;
	snapshot.lastRenderError = evidence.lastRenderError;
	snapshot.lastGlyphIndex = evidence.lastGlyphIndex;
	snapshot.lastBitmapWidth = evidence.lastBitmapWidth;
	snapshot.lastBitmapRows = evidence.lastBitmapRows;
	snapshot.redirectedFaceHasCharmap = evidence.redirectedFaceHasCharmap;
	snapshot.redirectedFaceGlyphs = evidence.redirectedFaceGlyphs;
	snapshot.sampleKoreanGlyph = evidence.sampleKoreanGlyph;
	snapshot.characterLookups = evidence.characterLookups;
	snapshot.characterLookupHits = evidence.characterLookupHits;
	snapshot.lastCharacter = evidence.lastCharacter;
	snapshot.lastLookupGlyph = evidence.lastLookupGlyph;
	std::memcpy(
		snapshot.lastLookupFamily, evidence.lastLookupFamily,
		sizeof(snapshot.lastLookupFamily));
	snapshot.lastLookupFamily[kUnityFontEvidenceFamilyChars - 1] = L'\0';
	snapshot.mappedCharacterLookups = evidence.mappedCharacterLookups;
	snapshot.mappedCharacterLookupHits = evidence.mappedCharacterLookupHits;
	snapshot.lastMappedCharacter = evidence.lastMappedCharacter;
	snapshot.lastMappedGlyph = evidence.lastMappedGlyph;
	snapshot.lastMappedSampleKoreanGlyph =
		evidence.lastMappedSampleKoreanGlyph;
	std::memcpy(
		snapshot.lastMappedFamily, evidence.lastMappedFamily,
		sizeof(snapshot.lastMappedFamily));
	snapshot.lastMappedFamily[kUnityFontEvidenceFamilyChars - 1] = L'\0';
	std::memcpy(
		snapshot.lastMappedResolvedFaceFamily,
		evidence.lastMappedResolvedFaceFamily,
		sizeof(snapshot.lastMappedResolvedFaceFamily));
	snapshot.lastMappedResolvedFaceFamily[
		kUnityFontEvidenceFamilyChars - 1] = L'\0';
	snapshot.faceResolutions = evidence.faceResolutions;
	snapshot.faceResolutionHits = evidence.faceResolutionHits;
	snapshot.faceResolutionGlyphHits = evidence.faceResolutionGlyphHits;
	snapshot.lastFaceResolutionGlyph = evidence.lastFaceResolutionGlyph;
	snapshot.lastFaceResolutionSampleKoreanGlyph =
		evidence.lastFaceResolutionSampleKoreanGlyph;
	snapshot.osFaceResolutions = evidence.osFaceResolutions;
	snapshot.osFaceResolutionHits = evidence.osFaceResolutionHits;
	snapshot.lastOsFaceSampleKoreanGlyph =
		evidence.lastOsFaceSampleKoreanGlyph;
	std::memcpy(
		snapshot.lastOsFaceFamily, evidence.lastOsFaceFamily,
		sizeof(snapshot.lastOsFaceFamily));
	snapshot.lastOsFaceFamily[kUnityFontEvidenceFamilyChars - 1] = L'\0';
	std::memcpy(
		snapshot.lastOsResolvedFaceFamily, evidence.lastOsResolvedFaceFamily,
		sizeof(snapshot.lastOsResolvedFaceFamily));
	snapshot.lastOsResolvedFaceFamily[
		kUnityFontEvidenceFamilyChars - 1] = L'\0';
	snapshot.mappedOsFaceResolutions = evidence.mappedOsFaceResolutions;
	snapshot.mappedOsFaceResolutionHits = evidence.mappedOsFaceResolutionHits;
	snapshot.lastMappedOsFaceSampleKoreanGlyph =
		evidence.lastMappedOsFaceSampleKoreanGlyph;
	std::memcpy(
		snapshot.lastMappedOsFaceFamily, evidence.lastMappedOsFaceFamily,
		sizeof(snapshot.lastMappedOsFaceFamily));
	snapshot.lastMappedOsFaceFamily[
		kUnityFontEvidenceFamilyChars - 1] = L'\0';
	std::memcpy(
		snapshot.lastMappedOsResolvedFaceFamily,
		evidence.lastMappedOsResolvedFaceFamily,
		sizeof(snapshot.lastMappedOsResolvedFaceFamily));
	snapshot.lastMappedOsResolvedFaceFamily[
		kUnityFontEvidenceFamilyChars - 1] = L'\0';
	std::memcpy(
		snapshot.observedPath, evidence.observedPath,
		sizeof(snapshot.observedPath));
	std::memcpy(
		snapshot.sourcePath, evidence.sourcePath,
		sizeof(snapshot.sourcePath));
	std::memcpy(
		snapshot.replacementPath, evidence.replacementPath,
		sizeof(snapshot.replacementPath));
	snapshot.sourcePath[kUnityFontEvidencePathChars - 1] = L'\0';
	snapshot.observedPath[kUnityFontEvidencePathChars - 1] = L'\0';
	snapshot.replacementPath[kUnityFontEvidencePathChars - 1] = L'\0';
	UnlockEvidence(mutableEvidence);
	return true;
}

bool FormatUnityFontEvidenceMappingName(
	DWORD pid,
	wchar_t* destination,
	std::size_t destinationChars) noexcept
{
	if (pid == 0 || destination == nullptr || destinationChars == 0)
		return false;
	destination[0] = L'\0';
	return SUCCEEDED(StringCchPrintfW(
		destination, destinationChars,
		L"Local\\MacType.UnityFont.%lu", pid));
}

}} // namespace renderer::unity
