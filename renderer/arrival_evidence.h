#pragma once

#include <cstdint>

namespace renderer {
namespace arrival_evidence {

struct ArrivalEvidence
{
	std::uint32_t gdiObjectsAtInstall;
	bool dwriteMappedBeforePin;
	bool dwriteCoreMappedBeforePin;
	bool visibleTopLevelWindowExisted;
};

struct Samplers
{
	std::uint32_t (*gdiObjectsAtInstall)() noexcept;
	bool (*dwriteMappedBeforePin)() noexcept;
	bool (*dwriteCoreMappedBeforePin)() noexcept;
	bool (*visibleTopLevelWindowExisted)() noexcept;
};

const ArrivalEvidence& Record(const Samplers& samplers) noexcept;
const ArrivalEvidence* Recorded() noexcept;
void CompleteDeferredSamples() noexcept;
Samplers ProcessSamplers() noexcept;
void ResetForTests() noexcept;

} // namespace arrival_evidence
} // namespace renderer
