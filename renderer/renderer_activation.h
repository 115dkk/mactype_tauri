#pragma once

#include "../shared/renderer_activation_contract.h"

#include <windows.h>

namespace renderer {

enum class RendererAdmission : unsigned char
{
	unavailable,
	active,
	quietSkip,
};

enum class RendererAdmissionReason : unsigned char
{
	none,
	processExcluded,
	processUnloadRequested,
};

void PublishRendererAdmission(
	RendererAdmission admission,
	RendererAdmissionReason reason = RendererAdmissionReason::none) noexcept;

} // namespace renderer

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:MacTypeQueryActivationEvidenceV1=_MacTypeQueryActivationEvidenceV1@4")
#endif

extern "C" __declspec(dllexport) DWORD WINAPI
MacTypeQueryActivationEvidenceV1(
	MacTypeRendererActivationEvidenceV1* evidence) noexcept;
