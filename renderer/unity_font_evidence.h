#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>

namespace renderer { namespace unity {

constexpr DWORD kUnityFontEvidenceSchema = 1;
constexpr std::size_t kUnityFontEvidencePathChars = 520;

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
bool ReadUnityFontEvidence(
	const UnityFontEvidenceV1& evidence,
	UnityFontEvidenceSnapshot& snapshot) noexcept;
bool FormatUnityFontEvidenceMappingName(
	DWORD pid,
	wchar_t* destination,
	std::size_t destinationChars) noexcept;

}} // namespace renderer::unity
