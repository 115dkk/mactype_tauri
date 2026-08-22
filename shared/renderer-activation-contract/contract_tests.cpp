#include "renderer_activation_contract.h"

#include <cassert>
#include <cstring>

namespace {

constexpr char kRuntimeGeneration[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr char kProfileDigest[] =
    "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

static_assert(sizeof(kRuntimeGeneration) == MACTYPE_RENDERER_RUNTIME_GENERATION_BYTES);
static_assert(sizeof(kProfileDigest) == MACTYPE_RENDERER_PROFILE_DIGEST_BYTES);

MacTypeRendererActivationEvidenceV1 Request()
{
    MacTypeRendererActivationEvidenceV1 request{};
    request.struct_size = MACTYPE_RENDERER_ACTIVATION_EVIDENCE_V1_SIZE;
    request.schema_version = MACTYPE_RENDERER_ACTIVATION_SCHEMA_VERSION;
    request.reason = MACTYPE_RENDERER_REASON_NONE;
    request.architecture = MACTYPE_RENDERER_ARCHITECTURE_X64;
    request.module_load = MACTYPE_RENDERER_MODULE_LOAD_LOADED_BY_REQUEST;
    request.pid = 42U;
    request.session_id = 2U;
    request.creation_time = UINT64_C(133967890123456789);
    std::memcpy(
        request.binding.runtime_generation_id,
        kRuntimeGeneration,
        sizeof(kRuntimeGeneration));
    std::memcpy(
        request.binding.profile_digest,
        kProfileDigest,
        sizeof(kProfileDigest));
    return request;
}

} // namespace

int main()
{
    MacTypeRendererActivationEvidenceV1 evidence = Request();
    assert(MacTypeValidateRendererActivationRequestV1(&evidence) != 0);
    MacTypeRendererActivationEvidenceV1 alreadyLoaded = evidence;
    alreadyLoaded.module_load = MACTYPE_RENDERER_MODULE_LOAD_ALREADY_LOADED;
    assert(MacTypeValidateRendererActivationRequestV1(&alreadyLoaded) != 0);
    assert(std::strcmp(
               MacTypeRendererActivationDispositionCode(
                   MACTYPE_RENDERER_DISPOSITION_QUIET_SKIP),
               "quiet-skip") == 0);
    assert(std::strcmp(
               MacTypeRendererCapabilityCode(
                   MACTYPE_RENDERER_CAPABILITY_DIRECT_WRITE_CORE),
               "direct-write-core") == 0);

    evidence.disposition = MACTYPE_RENDERER_DISPOSITION_ACTIVE;
    evidence.lifecycle_revision = 7U;
    evidence.capability_active =
        MACTYPE_RENDERER_CAPABILITY_GDI |
        MACTYPE_RENDERER_CAPABILITY_DIRECT_WRITE;
    evidence.capability_unavailable =
        MACTYPE_RENDERER_CAPABILITY_DIRECT_WRITE_CORE;
    std::memcpy(
        evidence.effective_profile_digest,
        kProfileDigest,
        sizeof(kProfileDigest));
    assert(MacTypeValidateRendererActivationEvidenceV1(&evidence) != 0);

    evidence.capability_unavailable = 0U;
    evidence.capability_failed = MACTYPE_RENDERER_CAPABILITY_DIRECT_WRITE_CORE;
    assert(MacTypeValidateRendererActivationEvidenceV1(&evidence) != 0);

    evidence.schema_version += 1U;
    assert(MacTypeValidateRendererActivationEvidenceV1(&evidence) == 0);
    return 0;
}
