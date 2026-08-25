#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>

namespace renderer { namespace unity {

constexpr DWORD kUnityFontEvidenceSchema = 1;
constexpr std::size_t kUnityFontEvidencePathChars = 520;
constexpr std::size_t kUnityFontEvidenceFamilyChars = 128;

struct UnityFontEvidenceV1 final
{
	DWORD structSize;
	DWORD schema;
	DWORD pid;
	volatile LONG writerLock;
	volatile LONG sequence;
	LONG redirectAttempts;
	LONG redirectSuccesses;
	LONG redirectFallbacks;
	LONG observedFontOpens;
	LONG renderCalls;
	LONG renderSuccesses;
	LONG nonEmptyBitmaps;
	LONG lastRenderError;
	LONG lastGlyphIndex;
	LONG lastBitmapWidth;
	LONG lastBitmapRows;
	LONG redirectedFaceHasCharmap;
	LONG redirectedFaceGlyphs;
	LONG sampleKoreanGlyph;
	LONG characterLookups;
	LONG characterLookupHits;
	LONG lastCharacter;
	LONG lastLookupGlyph;
	LONG faceResolutions;
	LONG faceResolutionHits;
	LONG faceResolutionGlyphHits;
	LONG lastFaceResolutionGlyph;
	LONG lastFaceResolutionSampleKoreanGlyph;
	LONG mappedCharacterLookups;
	LONG mappedCharacterLookupHits;
	LONG lastMappedCharacter;
	LONG lastMappedGlyph;
	LONG lastMappedSampleKoreanGlyph;
	LONG osFaceResolutions;
	LONG osFaceResolutionHits;
	LONG lastOsFaceSampleKoreanGlyph;
	LONG mappedOsFaceResolutions;
	LONG mappedOsFaceResolutionHits;
	LONG lastMappedOsFaceSampleKoreanGlyph;
	wchar_t lastLookupFamily[kUnityFontEvidenceFamilyChars];
	wchar_t lastMappedFamily[kUnityFontEvidenceFamilyChars];
	wchar_t lastMappedResolvedFaceFamily[kUnityFontEvidenceFamilyChars];
	wchar_t lastOsFaceFamily[kUnityFontEvidenceFamilyChars];
	wchar_t lastOsResolvedFaceFamily[kUnityFontEvidenceFamilyChars];
	wchar_t lastMappedOsFaceFamily[kUnityFontEvidenceFamilyChars];
	wchar_t lastMappedOsResolvedFaceFamily[kUnityFontEvidenceFamilyChars];
	wchar_t observedPath[kUnityFontEvidencePathChars];
	wchar_t sourcePath[kUnityFontEvidencePathChars];
	wchar_t replacementPath[kUnityFontEvidencePathChars];
};

struct UnityFontEvidenceSnapshot final
{
	DWORD pid = 0;
	LONG redirectAttempts = 0;
	LONG redirectSuccesses = 0;
	LONG redirectFallbacks = 0;
	LONG observedFontOpens = 0;
	LONG renderCalls = 0;
	LONG renderSuccesses = 0;
	LONG nonEmptyBitmaps = 0;
	LONG lastRenderError = 0;
	LONG lastGlyphIndex = 0;
	LONG lastBitmapWidth = 0;
	LONG lastBitmapRows = 0;
	LONG redirectedFaceHasCharmap = 0;
	LONG redirectedFaceGlyphs = 0;
	LONG sampleKoreanGlyph = 0;
	LONG characterLookups = 0;
	LONG characterLookupHits = 0;
	LONG lastCharacter = 0;
	LONG lastLookupGlyph = 0;
	LONG faceResolutions = 0;
	LONG faceResolutionHits = 0;
	LONG faceResolutionGlyphHits = 0;
	LONG lastFaceResolutionGlyph = 0;
	LONG lastFaceResolutionSampleKoreanGlyph = 0;
	LONG mappedCharacterLookups = 0;
	LONG mappedCharacterLookupHits = 0;
	LONG lastMappedCharacter = 0;
	LONG lastMappedGlyph = 0;
	LONG lastMappedSampleKoreanGlyph = 0;
	LONG osFaceResolutions = 0;
	LONG osFaceResolutionHits = 0;
	LONG lastOsFaceSampleKoreanGlyph = 0;
	LONG mappedOsFaceResolutions = 0;
	LONG mappedOsFaceResolutionHits = 0;
	LONG lastMappedOsFaceSampleKoreanGlyph = 0;
	wchar_t lastLookupFamily[kUnityFontEvidenceFamilyChars]{};
	wchar_t lastMappedFamily[kUnityFontEvidenceFamilyChars]{};
	wchar_t lastMappedResolvedFaceFamily[kUnityFontEvidenceFamilyChars]{};
	wchar_t lastOsFaceFamily[kUnityFontEvidenceFamilyChars]{};
	wchar_t lastOsResolvedFaceFamily[kUnityFontEvidenceFamilyChars]{};
	wchar_t lastMappedOsFaceFamily[kUnityFontEvidenceFamilyChars]{};
	wchar_t lastMappedOsResolvedFaceFamily[kUnityFontEvidenceFamilyChars]{};
	wchar_t observedPath[kUnityFontEvidencePathChars]{};
	wchar_t sourcePath[kUnityFontEvidencePathChars]{};
	wchar_t replacementPath[kUnityFontEvidencePathChars]{};
};

void InitializeUnityFontEvidence(
	UnityFontEvidenceV1& evidence,
	DWORD pid) noexcept;
void RecordUnityFontRedirect(
	UnityFontEvidenceV1& evidence,
	const wchar_t* sourcePath,
	const wchar_t* replacementPath,
	bool succeeded) noexcept;
void RecordUnityFontFileOpen(
	UnityFontEvidenceV1& evidence,
	const wchar_t* path) noexcept;
void RecordUnityFontRender(
	UnityFontEvidenceV1& evidence,
	LONG error,
	unsigned int glyphIndex,
	unsigned int bitmapWidth,
	unsigned int bitmapRows) noexcept;
void RecordUnityFontFaceDetails(
	UnityFontEvidenceV1& evidence,
	bool hasCharmap,
	LONG faceGlyphs,
	unsigned int sampleKoreanGlyph) noexcept;
void RecordUnityFontCharacterLookup(
	UnityFontEvidenceV1& evidence,
	const wchar_t* family,
	bool mapped,
	unsigned int character,
	bool found,
	unsigned int glyphIndex,
	unsigned int sampleKoreanGlyph,
	const wchar_t* resolvedFaceFamily) noexcept;
void RecordUnityFontFaceResolution(
	UnityFontEvidenceV1& evidence,
	bool found,
	unsigned int glyphIndex,
	unsigned int sampleKoreanGlyph) noexcept;
void RecordUnityFontOsFaceResolution(
	UnityFontEvidenceV1& evidence,
	const wchar_t* family,
	bool mapped,
	bool found,
	unsigned int sampleKoreanGlyph,
	const wchar_t* resolvedFaceFamily) noexcept;
bool ReadUnityFontEvidence(
	const UnityFontEvidenceV1& evidence,
	UnityFontEvidenceSnapshot& snapshot) noexcept;
bool FormatUnityFontEvidenceMappingName(
	DWORD pid,
	wchar_t* destination,
	std::size_t destinationChars) noexcept;

}} // namespace renderer::unity
