#pragma once

namespace mactype {
namespace hooking {

enum class HookCompatibility : unsigned char
{
    compatible,
    dynamic_code_prohibited,
    binary_signature_restricted,
};

struct HookCompatibilityEvidence final
{
    bool dynamic_code_query_succeeded{};
    bool prohibit_dynamic_code{};
    bool allow_thread_opt_out{};
    bool signature_query_succeeded{};
    bool microsoft_signed_only{};
    bool store_signed_only{};
    bool mitigation_opt_in{};
};

inline HookCompatibility ClassifyHookCompatibility(
    HookCompatibilityEvidence const& evidence) noexcept
{
    if (evidence.signature_query_succeeded &&
        (evidence.microsoft_signed_only || evidence.store_signed_only ||
         evidence.mitigation_opt_in))
        return HookCompatibility::binary_signature_restricted;
    if (evidence.dynamic_code_query_succeeded &&
        evidence.prohibit_dynamic_code && !evidence.allow_thread_opt_out)
        return HookCompatibility::dynamic_code_prohibited;
    return HookCompatibility::compatible;
}

inline char const* HookCompatibilityCode(
    HookCompatibility compatibility) noexcept
{
    switch (compatibility)
    {
    case HookCompatibility::compatible:
        return "hook-compatible";
    case HookCompatibility::dynamic_code_prohibited:
        return "dynamic-code-policy-blocks-hooks";
    case HookCompatibility::binary_signature_restricted:
        return "binary-signature-policy-blocks-module";
    }
    return "hook-compatible";
}

} // namespace hooking
} // namespace mactype
