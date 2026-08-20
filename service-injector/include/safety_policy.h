#pragma once

#include "hook_compatibility.h"

#include <string_view>

namespace mactype::injector {

using HookCompatibility = hooking::HookCompatibility;
using HookCompatibilityEvidence = hooking::HookCompatibilityEvidence;

[[nodiscard]] bool protection_state_allows_injection(bool query_succeeded,
                                                     bool is_unprotected) noexcept;
[[nodiscard]] HookCompatibility classify_hook_compatibility(
    const HookCompatibilityEvidence& evidence) noexcept;
[[nodiscard]] std::string_view hook_compatibility_code(
    HookCompatibility compatibility) noexcept;

}  // namespace mactype::injector
