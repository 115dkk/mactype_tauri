#include "safety_policy.h"

namespace mactype::injector {

bool protection_state_allows_injection(const bool query_succeeded,
                                       const bool is_unprotected) noexcept {
    return query_succeeded && is_unprotected;
}

HookCompatibility classify_hook_compatibility(
    const HookCompatibilityEvidence& evidence) noexcept {
    return hooking::ClassifyHookCompatibility(evidence);
}

std::string_view hook_compatibility_code(
    const HookCompatibility compatibility) noexcept {
    return hooking::HookCompatibilityCode(compatibility);
}

}  // namespace mactype::injector
