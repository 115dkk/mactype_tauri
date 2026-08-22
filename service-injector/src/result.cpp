#include "result.h"

#include <stdexcept>

namespace mactype::injector {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

std::string_view status_name(const ResultStatus status) noexcept {
    switch (status) {
        case ResultStatus::injected:
            return "injected";
        case ResultStatus::skipped:
            return "skipped";
        case ResultStatus::rejected:
            return "rejected";
        case ResultStatus::failed:
            return "failed";
        case ResultStatus::integrity_failed:
            return "integrity";
        case ResultStatus::timed_out:
            return "timeout";
    }
    return "failed";
}

void append_renderer_evidence(
    std::string& json,
    const std::optional<MacTypeRendererActivationEvidenceV1>& evidence) {
    json += ",\"rendererEvidence\":";
    if (!evidence) {
        json += "null";
        return;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&*evidence);
    json.push_back('"');
    for (std::size_t index = 0; index < sizeof(*evidence); ++index) {
        json.push_back(kHexDigits[bytes[index] >> 4U]);
        json.push_back(kHexDigits[bytes[index] & 0x0fU]);
    }
    json.push_back('"');
}

}  // namespace

Result make_result(const BrokerRequest& request, const ResultStatus status,
                   const std::string_view code, const std::string_view module,
                   const std::uint32_t windows_error, const bool cleanup_complete) {
    return Result{status,
                  code,
                  request.pid,
                  request.expected_session_id,
                  request.generation_id,
                  module,
                  windows_error,
                  cleanup_complete,
                  std::nullopt};
}

std::string to_json(const Result& result) {
    std::string json;
    json.reserve(1'024U);
    json += "{\"schemaVersion\":2,\"status\":\"";
    json += status_name(result.status);
    json += "\",\"code\":\"";
    json += result.code;
    json += "\",\"pid\":";
    json += std::to_string(result.pid);
    json += ",\"sessionId\":";
    json += std::to_string(result.session_id);
    json += ",\"generationId\":\"";
    json += result.generation_id;
    json += "\",\"module\":\"";
    json += result.module;
    json += "\",\"windowsError\":";
    json += std::to_string(result.windows_error);
    json += ",\"cleanupComplete\":";
    json += result.cleanup_complete ? "true" : "false";
    append_renderer_evidence(json, result.renderer_evidence);
    json.push_back('}');
    if (json.size() > 1'536U) {
        throw std::length_error{"injector result exceeded its public bound"};
    }
    return json;
}

int exit_code(const ResultStatus status) noexcept {
    switch (status) {
        case ResultStatus::injected:
        case ResultStatus::skipped:
            return 0;
        case ResultStatus::rejected:
            return 2;
        case ResultStatus::failed:
        case ResultStatus::integrity_failed:
            return 3;
        case ResultStatus::timed_out:
            return 4;
    }
    return 3;
}

}  // namespace mactype::injector
