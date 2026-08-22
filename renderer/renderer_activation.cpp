#include "renderer_activation.h"

#include "hook_lifecycle.h"
#include "profile_runtime.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace renderer {
namespace {

struct AdmissionState final
{
	RendererAdmission admission = RendererAdmission::unavailable;
	RendererAdmissionReason reason = RendererAdmissionReason::none;
};

std::atomic<unsigned int> g_admissionState{0};

unsigned int EncodeAdmission(
	RendererAdmission admission,
	RendererAdmissionReason reason) noexcept
{
	return static_cast<unsigned int>(admission) |
		(static_cast<unsigned int>(reason) << 8U);
}

AdmissionState LoadAdmission() noexcept
{
	const unsigned int encoded =
		g_admissionState.load(std::memory_order_acquire);
	return {
		static_cast<RendererAdmission>(encoded & 0xffU),
		static_cast<RendererAdmissionReason>((encoded >> 8U) & 0xffU)};
}

std::uint64_t CapabilityBit(HookCapability capability) noexcept
{
	switch (capability)
	{
	case HookCapability::gdi:
		return MACTYPE_RENDERER_CAPABILITY_GDI;
	case HookCapability::childInjection:
		return MACTYPE_RENDERER_CAPABILITY_CHILD_INJECTION;
	case HookCapability::directWrite:
		return MACTYPE_RENDERER_CAPABILITY_DIRECT_WRITE;
	case HookCapability::directWriteCore:
		return MACTYPE_RENDERER_CAPABILITY_DIRECT_WRITE_CORE;
	case HookCapability::direct2D:
		return MACTYPE_RENDERER_CAPABILITY_DIRECT2D;
	case HookCapability::gdiPlus:
		return MACTYPE_RENDERER_CAPABILITY_GDI_PLUS;
	case HookCapability::fontSubstitution:
		return MACTYPE_RENDERER_CAPABILITY_FONT_SUBSTITUTION;
	}
	return 0;
}

void ProjectCapabilities(
	const HookLifecycleSnapshot& lifecycle,
	MacTypeRendererActivationEvidenceV1& evidence) noexcept
{
	for (const CapabilityRecord& capability : lifecycle.capabilities)
	{
		const std::uint64_t bit = CapabilityBit(capability.capability);
		switch (capability.state)
		{
		case CapabilityState::active:
			evidence.capability_active |= bit;
			break;
		case CapabilityState::failed:
			evidence.capability_failed |= bit;
			break;
		case CapabilityState::unavailable:
		case CapabilityState::stopped:
			evidence.capability_unavailable |= bit;
			break;
		case CapabilityState::unknown:
		case CapabilityState::pending:
			break;
		}
	}
	evidence.capability_failed &= ~evidence.capability_active;
	evidence.capability_unavailable &=
		~(evidence.capability_active | evidence.capability_failed);
}

MacTypeRendererActivationReason AdmissionReasonCode(
	RendererAdmissionReason reason) noexcept
{
	switch (reason)
	{
	case RendererAdmissionReason::processExcluded:
		return MACTYPE_RENDERER_REASON_PROCESS_EXCLUDED;
	case RendererAdmissionReason::processUnloadRequested:
		return MACTYPE_RENDERER_REASON_PROCESS_UNLOAD_REQUESTED;
	case RendererAdmissionReason::none:
		return MACTYPE_RENDERER_REASON_NONE;
	}
	return MACTYPE_RENDERER_REASON_EVIDENCE_UNAVAILABLE;
}

bool CurrentProcessMatches(
	const MacTypeRendererActivationEvidenceV1& request) noexcept
{
	const DWORD pid = GetCurrentProcessId();
	DWORD session = 0;
	FILETIME created{};
	FILETIME exited{};
	FILETIME kernel{};
	FILETIME user{};
	if (request.pid != pid || !ProcessIdToSessionId(pid, &session) ||
		request.session_id != session ||
		!GetProcessTimes(
			GetCurrentProcess(), &created, &exited, &kernel, &user))
		return false;
	ULARGE_INTEGER creation{};
	creation.LowPart = created.dwLowDateTime;
	creation.HighPart = created.dwHighDateTime;
	if (request.creation_time != creation.QuadPart)
		return false;
#if defined(_WIN64)
	return request.architecture == MACTYPE_RENDERER_ARCHITECTURE_X64;
#else
	return request.architecture == MACTYPE_RENDERER_ARCHITECTURE_X86;
#endif
}

void PublishFailure(
	MacTypeRendererActivationEvidenceV1& evidence,
	MacTypeRendererActivationReason reason) noexcept
{
	evidence.disposition = MACTYPE_RENDERER_DISPOSITION_FAILED;
	evidence.reason = reason;
}

} // namespace

void PublishRendererAdmission(
	RendererAdmission admission,
	RendererAdmissionReason reason) noexcept
{
	g_admissionState.store(
		EncodeAdmission(admission, reason), std::memory_order_release);
}

} // namespace renderer

extern "C" DWORD WINAPI MacTypeQueryActivationEvidenceV1(
	MacTypeRendererActivationEvidenceV1* evidence) noexcept
{
	if (!MacTypeValidateRendererActivationRequestV1(evidence))
		return ERROR_INVALID_DATA;
	const MacTypeRendererActivationEvidenceV1 request = *evidence;
	if (!renderer::CurrentProcessMatches(request))
		return ERROR_INVALID_PARAMETER;

	MacTypeRendererActivationEvidenceV1 result = request;
	const renderer::RendererPolicyRef policy =
		renderer::CurrentRendererPolicy();
	const renderer::HookLifecycleSnapshot lifecycle =
		renderer::ProcessHookCoordinator().Snapshot();
	result.lifecycle_revision = lifecycle.revision;
	renderer::ProjectCapabilities(lifecycle, result);

	if (!policy ||
		policy->profile_digest().size() !=
			MACTYPE_RENDERER_PROFILE_DIGEST_CHARS)
	{
		renderer::PublishFailure(
			result, MACTYPE_RENDERER_REASON_EVIDENCE_UNAVAILABLE);
	}
	else
	{
		std::memcpy(
			result.effective_profile_digest,
			policy->profile_digest().data(),
			policy->profile_digest().size());
		if (!MacTypeRendererProfileDigestIsCanonical(
				result.effective_profile_digest))
		{
			renderer::PublishFailure(
				result, MACTYPE_RENDERER_REASON_PROFILE_INVALID);
		}
		else if (std::memcmp(
				result.effective_profile_digest,
				result.binding.profile_digest,
				MACTYPE_RENDERER_PROFILE_DIGEST_BYTES) != 0)
		{
			renderer::PublishFailure(
				result, MACTYPE_RENDERER_REASON_PROFILE_DIGEST_MISMATCH);
		}
		else if (lifecycle.phase == renderer::RuntimePhase::stopping)
		{
			renderer::PublishFailure(
				result, MACTYPE_RENDERER_REASON_LIFECYCLE_STOPPING);
		}
		else if (lifecycle.phase != renderer::RuntimePhase::active ||
			lifecycle.revision == 0)
		{
			renderer::PublishFailure(
				result, MACTYPE_RENDERER_REASON_INITIALIZATION_FAILED);
		}
		else
		{
			const renderer::AdmissionState admission =
				renderer::LoadAdmission();
			if (admission.admission == renderer::RendererAdmission::active)
			{
				result.disposition = MACTYPE_RENDERER_DISPOSITION_ACTIVE;
				result.reason = MACTYPE_RENDERER_REASON_NONE;
			}
			else if (
				admission.admission == renderer::RendererAdmission::quietSkip &&
				result.capability_active == 0 &&
				result.capability_failed == 0)
			{
				result.disposition = MACTYPE_RENDERER_DISPOSITION_QUIET_SKIP;
				result.reason =
					renderer::AdmissionReasonCode(admission.reason);
			}
			else
			{
				renderer::PublishFailure(
					result, MACTYPE_RENDERER_REASON_EVIDENCE_UNAVAILABLE);
			}
		}
	}

	if (!MacTypeValidateRendererActivationEvidenceV1(&result))
		return ERROR_INVALID_DATA;
	*evidence = result;
	return ERROR_SUCCESS;
}
