#pragma once

#include "broker_request.h"
#include "operation_deadline.h"
#include "result.h"

#include <windows.h>

#include <filesystem>

namespace mactype::injector {

enum class RendererLoadOrigin {
    loaded_by_request,
    already_loaded,
};

[[nodiscard]] Result bind_renderer_activation_evidence(
    HANDLE process, const BrokerRequest& request,
    const std::filesystem::path& module_path, Result module_result,
    RendererLoadOrigin load_origin, const OperationDeadline& deadline) noexcept;

}  // namespace mactype::injector
