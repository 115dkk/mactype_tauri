#include "../../../renderer/hook_lifecycle.h"
#include "../../../renderer/profile_runtime.h"
#include "../../../renderer/renderer_activation.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kRuntimeGeneration =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string ProfileDigest(char digit)
{
    return "sha256:" + std::string(64, digit);
}

renderer::RendererPolicyCandidate Candidate(const std::string& profileDigest)
{
    renderer::RendererPolicyCandidate candidate;
    candidate.valid = true;
    candidate.profileDigest = profileDigest;
    candidate.freeType.cacheMaxFaces = 64;
    candidate.freeType.cacheMaxSizes = 1200;
    candidate.freeType.cacheMaxBytes = 10 * 1024 * 1024;
    candidate.raster.gamma = 1.0f;
    candidate.directWrite.gamma = 1.0f;
    candidate.directWrite.contrast = 1.0f;
    candidate.directWrite.clearTypeLevel = 1.0f;
    return candidate;
}

MacTypeRendererActivationEvidenceV1 RequestFor(
    const std::string& profileDigest)
{
    Require(
        kRuntimeGeneration.size() == MACTYPE_RENDERER_RUNTIME_GENERATION_CHARS,
        "the runtime generation fixture must match the generated contract");
    Require(
        profileDigest.size() == MACTYPE_RENDERER_PROFILE_DIGEST_CHARS,
        "the profile digest fixture must match the generated contract");

    MacTypeRendererActivationEvidenceV1 request{};
    request.struct_size = MACTYPE_RENDERER_ACTIVATION_EVIDENCE_V1_SIZE;
    request.schema_version = MACTYPE_RENDERER_ACTIVATION_SCHEMA_VERSION;
    request.reason = MACTYPE_RENDERER_REASON_NONE;
#if defined(_WIN64)
    request.architecture = MACTYPE_RENDERER_ARCHITECTURE_X64;
#else
    request.architecture = MACTYPE_RENDERER_ARCHITECTURE_X86;
#endif
    request.module_load = MACTYPE_RENDERER_MODULE_LOAD_LOADED_BY_REQUEST;
    request.pid = static_cast<std::uint32_t>(GetCurrentProcessId());

    DWORD sessionId = 0;
    Require(
        ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) != FALSE,
        "the current process session must be observable");
    request.session_id = static_cast<std::uint32_t>(sessionId);

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    Require(
        GetProcessTimes(
            GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE,
        "the current process creation time must be observable");
    ULARGE_INTEGER creation{};
    creation.LowPart = created.dwLowDateTime;
    creation.HighPart = created.dwHighDateTime;
    request.creation_time = creation.QuadPart;

    std::memcpy(
        request.binding.runtime_generation_id,
        kRuntimeGeneration.data(),
        kRuntimeGeneration.size());
    std::memcpy(
        request.binding.profile_digest,
        profileDigest.data(),
        profileDigest.size());
    return request;
}

void RequireCurrentIdentity(
    const MacTypeRendererActivationEvidenceV1& evidence)
{
    const MacTypeRendererActivationEvidenceV1 current =
        RequestFor(evidence.binding.profile_digest);
    Require(
        evidence.pid == current.pid &&
            evidence.creation_time == current.creation_time &&
            evidence.session_id == current.session_id &&
            evidence.architecture == current.architecture,
        "activation evidence must retain the current process identity");
}

void RequireCanonicalBinding(
    const MacTypeRendererActivationEvidenceV1& evidence)
{
    Require(
        MacTypeRendererRuntimeGenerationIsCanonical(
            evidence.binding.runtime_generation_id) != 0 &&
            MacTypeRendererProfileDigestIsCanonical(
                evidence.binding.profile_digest) != 0,
        "activation evidence must retain a canonical runtime binding");
}

} // namespace

int main()
{
    const std::string activeDigest = ProfileDigest('a');
    const std::string mismatchedDigest = ProfileDigest('b');

    const renderer::ProfilePublication publication =
        renderer::ProcessProfileRuntime().Publish(Candidate(activeDigest));
    Require(
        publication.published() && publication.snapshot &&
            publication.snapshot->profile_digest() == activeDigest,
        "the focused activation test must publish one complete profile");

    renderer::HookCoordinator& lifecycle = renderer::ProcessHookCoordinator();
    Require(lifecycle.BeginStart(), "the activation lifecycle must begin");
    Require(lifecycle.CompleteStart(), "the activation lifecycle must become active");
    const renderer::HookLifecycleSnapshot activeLifecycle = lifecycle.Snapshot();
    Require(
        activeLifecycle.revision != 0 &&
            activeLifecycle.phase == renderer::RuntimePhase::active,
        "the active lifecycle must expose a non-zero revision");

    renderer::PublishRendererAdmission(renderer::RendererAdmission::active);
    const MacTypeRendererActivationEvidenceV1 activeRequest =
        RequestFor(activeDigest);
    Require(
        MacTypeValidateRendererActivationRequestV1(&activeRequest) != 0,
        "the generated current-process request must satisfy the wire contract");
    RequireCurrentIdentity(activeRequest);
    RequireCanonicalBinding(activeRequest);

    MacTypeRendererActivationEvidenceV1 activeEvidence = activeRequest;
    Require(
        MacTypeQueryActivationEvidenceV1(&activeEvidence) == ERROR_SUCCESS,
        "an active renderer must answer a canonical current-process request");
    Require(
        MacTypeValidateRendererActivationEvidenceV1(&activeEvidence) != 0,
        "active renderer evidence must satisfy the generated wire contract");
    Require(
        activeEvidence.disposition == MACTYPE_RENDERER_DISPOSITION_ACTIVE &&
            activeEvidence.reason == MACTYPE_RENDERER_REASON_NONE,
        "active admission must publish an Active disposition without a reason");
    Require(
        std::memcmp(
            activeEvidence.effective_profile_digest,
            activeRequest.binding.profile_digest,
            MACTYPE_RENDERER_PROFILE_DIGEST_BYTES) == 0,
        "active evidence must bind to the published profile digest");
    Require(
        activeEvidence.lifecycle_revision == activeLifecycle.revision,
        "active evidence must publish the current hook lifecycle revision");
    Require(
        std::memcmp(
            &activeEvidence.binding,
            &activeRequest.binding,
            sizeof(activeEvidence.binding)) == 0,
        "the export must preserve the caller's canonical runtime binding");
    RequireCurrentIdentity(activeEvidence);
    RequireCanonicalBinding(activeEvidence);

    MacTypeRendererActivationEvidenceV1 mismatchEvidence =
        RequestFor(mismatchedDigest);
    Require(
        MacTypeQueryActivationEvidenceV1(&mismatchEvidence) == ERROR_SUCCESS,
        "a profile mismatch must return structured activation evidence");
    Require(
        MacTypeValidateRendererActivationEvidenceV1(&mismatchEvidence) != 0,
        "profile-mismatch evidence must satisfy the generated wire contract");
    Require(
        mismatchEvidence.disposition == MACTYPE_RENDERER_DISPOSITION_FAILED &&
            mismatchEvidence.reason ==
                MACTYPE_RENDERER_REASON_PROFILE_DIGEST_MISMATCH,
        "a request for another profile must fail with ProfileDigestMismatch");
    Require(
        std::memcmp(
            mismatchEvidence.effective_profile_digest,
            activeDigest.c_str(),
            MACTYPE_RENDERER_PROFILE_DIGEST_BYTES) == 0,
        "profile-mismatch evidence must report the renderer's effective profile");

    renderer::PublishRendererAdmission(
        renderer::RendererAdmission::quietSkip,
        renderer::RendererAdmissionReason::processExcluded);
    MacTypeRendererActivationEvidenceV1 quietEvidence =
        RequestFor(activeDigest);
    Require(
        MacTypeQueryActivationEvidenceV1(&quietEvidence) == ERROR_SUCCESS,
        "an excluded process must return structured quiet-skip evidence");
    Require(
        MacTypeValidateRendererActivationEvidenceV1(&quietEvidence) != 0,
        "quiet-skip evidence must satisfy the generated wire contract");
    Require(
        quietEvidence.disposition == MACTYPE_RENDERER_DISPOSITION_QUIET_SKIP &&
            quietEvidence.reason == MACTYPE_RENDERER_REASON_PROCESS_EXCLUDED,
        "explicit process exclusion must publish QuietSkip/ProcessExcluded");
    Require(
        quietEvidence.capability_active == 0 &&
            quietEvidence.capability_failed == 0,
        "quiet skip must not claim active or failed hook capabilities");
    Require(
        quietEvidence.lifecycle_revision == activeLifecycle.revision,
        "quiet-skip evidence must retain the current lifecycle revision");
    Require(
        !renderer::CurrentRendererPolicy(),
        "quiet skip must release the published renderer policy before unload");
    Require(
        lifecycle.phase() == renderer::RuntimePhase::uninitialized,
        "quiet skip must release lifecycle storage before unload");
    Require(
        !renderer::font_substitution::ProcessRegistry().Load(),
        "quiet skip must release the substitution snapshot before unload");

    MacTypeRendererActivationEvidenceV1 malformed = RequestFor(activeDigest);
    malformed.struct_size = 0;
    Require(
        MacTypeValidateRendererActivationRequestV1(&malformed) == 0,
        "the malformed fixture must be rejected by the generated validator");
    Require(
        MacTypeQueryActivationEvidenceV1(&malformed) == ERROR_INVALID_DATA,
        "the renderer export must reject a malformed request");
    Require(
        MacTypeQueryActivationEvidenceV1(
            reinterpret_cast<MacTypeRendererActivationEvidenceV1*>(1)) !=
            ERROR_SUCCESS,
        "the renderer export must reject an unreadable caller pointer without crashing");

    std::cout << "Renderer activation round-trip tests passed.\n";
    return 0;
}
