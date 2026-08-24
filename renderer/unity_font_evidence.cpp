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
