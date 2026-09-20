#include "arrival_evidence.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

namespace renderer {
namespace arrival_evidence {
namespace {

constexpr unsigned char kEmpty = 0;
constexpr unsigned char kRecording = 1;
constexpr unsigned char kVisibleSamplePending = 2;
constexpr unsigned char kRecorded = 3;

std::atomic<unsigned char> recordState{kEmpty};
ArrivalEvidence processEvidence{};
bool (*pendingVisibleSampler)() noexcept = nullptr;

std::uint32_t SampleGdiObjects() noexcept
{
	return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

bool SampleDwriteMapped() noexcept
{
	return GetModuleHandleW(L"dwrite.dll") != nullptr;
}

bool SampleDwriteCoreMapped() noexcept
{
	return GetModuleHandleW(L"DWriteCore.dll") != nullptr;
}

struct VisibleWindowContext
{
	DWORD processId;
	bool found;
};

BOOL CALLBACK FindVisibleProcessWindow(HWND window, LPARAM parameter) noexcept
{
	auto* context = reinterpret_cast<VisibleWindowContext*>(parameter);
	DWORD processId = 0;
	GetWindowThreadProcessId(window, &processId);
	if (processId == context->processId && IsWindowVisible(window))
	{
		context->found = true;
		return FALSE;
	}
	return TRUE;
}

bool SampleVisibleTopLevelWindow() noexcept
{
	VisibleWindowContext context{GetCurrentProcessId(), false};
	EnumWindows(
		FindVisibleProcessWindow,
		reinterpret_cast<LPARAM>(&context));
	return context.found;
}

bool DeferVisibleTopLevelWindowSample() noexcept
{
	return false;
}

} // namespace

const ArrivalEvidence& Record(const Samplers& samplers) noexcept
{
	unsigned char expected = kEmpty;
	if (recordState.compare_exchange_strong(
			expected, kRecording, std::memory_order_acq_rel))
	{
		bool const deferVisibleSample =
			samplers.visibleTopLevelWindowExisted ==
			DeferVisibleTopLevelWindowSample;
		ArrivalEvidence const sampled{
			samplers.gdiObjectsAtInstall == nullptr
				? 0U : samplers.gdiObjectsAtInstall(),
			samplers.dwriteMappedBeforePin != nullptr &&
				samplers.dwriteMappedBeforePin(),
			samplers.dwriteCoreMappedBeforePin != nullptr &&
				samplers.dwriteCoreMappedBeforePin(),
			!deferVisibleSample &&
				samplers.visibleTopLevelWindowExisted != nullptr &&
				samplers.visibleTopLevelWindowExisted(),
		};
		processEvidence = sampled;
		pendingVisibleSampler = deferVisibleSample
			? SampleVisibleTopLevelWindow : nullptr;
		recordState.store(
			deferVisibleSample ? kVisibleSamplePending : kRecorded,
			std::memory_order_release);
	}
	else
	{
		while (recordState.load(std::memory_order_acquire) == kRecording)
		{
			YieldProcessor();
		}
	}
	return processEvidence;
}

const ArrivalEvidence* Recorded() noexcept
{
	unsigned char state = recordState.load(std::memory_order_acquire);
	while (state == kRecording)
	{
		YieldProcessor();
		state = recordState.load(std::memory_order_acquire);
	}
	return state == kRecorded || state == kVisibleSamplePending
		? &processEvidence : nullptr;
}

void CompleteDeferredSamples() noexcept
{
	unsigned char expected = kVisibleSamplePending;
	if (!recordState.compare_exchange_strong(
			expected, kRecording, std::memory_order_acq_rel))
	{
		return;
	}
	processEvidence.visibleTopLevelWindowExisted =
		pendingVisibleSampler != nullptr && pendingVisibleSampler();
	pendingVisibleSampler = nullptr;
	recordState.store(kRecorded, std::memory_order_release);
}

Samplers ProcessSamplers() noexcept
{
	return Samplers{
		SampleGdiObjects,
		SampleDwriteMapped,
		SampleDwriteCoreMapped,
		DeferVisibleTopLevelWindowSample,
	};
}

void ResetForTests() noexcept
{
	processEvidence = ArrivalEvidence{};
	pendingVisibleSampler = nullptr;
	recordState.store(kEmpty, std::memory_order_release);
}

} // namespace arrival_evidence
} // namespace renderer
