#include "renderer_activation_contract.h"

#include <string.h>

static const char k_runtime_generation[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char k_profile_digest[] =
    "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

int main(void)
{
    MacTypeRendererActivationEvidenceV1 request = {0};
    request.struct_size = MACTYPE_RENDERER_ACTIVATION_EVIDENCE_V1_SIZE;
    request.schema_version = MACTYPE_RENDERER_ACTIVATION_SCHEMA_VERSION;
    request.reason = MACTYPE_RENDERER_REASON_NONE;
    request.architecture = MACTYPE_RENDERER_ARCHITECTURE_X86;
    request.module_load = MACTYPE_RENDERER_MODULE_LOAD_LOADED_BY_REQUEST;
    request.pid = 42U;
    request.session_id = 2U;
    request.creation_time = UINT64_C(133967890123456789);
    memcpy(
        request.binding.runtime_generation_id,
        k_runtime_generation,
        sizeof(k_runtime_generation));
    memcpy(
        request.binding.profile_digest,
        k_profile_digest,
        sizeof(k_profile_digest));
    return MacTypeValidateRendererActivationRequestV1(&request) != 0 ? 0 : 1;
}
