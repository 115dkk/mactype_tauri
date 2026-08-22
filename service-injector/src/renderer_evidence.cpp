#include "renderer_evidence.h"

#include "module_inventory.h"
#include "unique_handle.h"

#include <psapi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace mactype::injector {
namespace {

constexpr DWORD kEvidenceTimeoutMs = 5'000U;
constexpr DWORD kQuietUnloadReserveMs = 3'000U;

class LocalModule final {
public:
    explicit LocalModule(HMODULE module) noexcept : module_{module} {}
    ~LocalModule() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }
    LocalModule(const LocalModule&) = delete;
    LocalModule& operator=(const LocalModule&) = delete;
    [[nodiscard]] HMODULE get() const noexcept { return module_; }

private:
    HMODULE module_{};
};

class RemoteEvidence final {
public:
    RemoteEvidence(HANDLE process, void* address) noexcept
        : process_{process}, address_{address} {}
    ~RemoteEvidence() {
        if (process_ != nullptr && address_ != nullptr) {
            VirtualFreeEx(process_, address_, 0U, MEM_RELEASE);
        }
    }
    RemoteEvidence(const RemoteEvidence&) = delete;
    RemoteEvidence& operator=(const RemoteEvidence&) = delete;
    [[nodiscard]] void* get() const noexcept { return address_; }
    [[nodiscard]] bool release() noexcept {
        if (process_ == nullptr || address_ == nullptr) {
            return true;
        }
        if (!VirtualFreeEx(process_, address_, 0U, MEM_RELEASE)) {
            return false;
        }
        process_ = nullptr;
        address_ = nullptr;
        return true;
    }
    void abandon() noexcept {
        process_ = nullptr;
        address_ = nullptr;
    }

private:
    HANDLE process_{};
    void* address_{};
};

enum class QueryState {
    complete,
    failed,
    cleanup_unknown,
};

enum class ReferenceReleaseState {
    complete,
    cleanup_unknown,
};

struct ReferenceReleaseResult final {
    ReferenceReleaseState state{ReferenceReleaseState::cleanup_unknown};
    FixedModuleState module_state{FixedModuleState::InventoryUnavailable};
    DWORD windows_error{};
};

struct QueryResult final {
    QueryState state{QueryState::failed};
    std::optional<MacTypeRendererActivationEvidenceV1> evidence;
    std::string_view code{"renderer-evidence-unavailable"};
    DWORD windows_error{};
};

[[nodiscard]] std::optional<std::uintptr_t> local_export_offset(
    const std::filesystem::path& module_path) noexcept {
    const LocalModule module{LoadLibraryExW(module_path.c_str(), nullptr,
                                            DONT_RESOLVE_DLL_REFERENCES)};
    if (module.get() == nullptr) {
        return std::nullopt;
    }
    const FARPROC procedure = GetProcAddress(
        module.get(), MACTYPE_RENDERER_ACTIVATION_QUERY_EXPORT);
    if (procedure == nullptr) {
        return std::nullopt;
    }
    MODULEINFO information{};
    if (!K32GetModuleInformation(GetCurrentProcess(), module.get(), &information,
                                 sizeof(information)) ||
        information.lpBaseOfDll == nullptr || information.SizeOfImage == 0U) {
        return std::nullopt;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(information.lpBaseOfDll);
    const auto address = reinterpret_cast<std::uintptr_t>(procedure);
    if (address < base || address - base >= information.SizeOfImage) {
        return std::nullopt;
    }
    return address - base;
}

[[nodiscard]] MacTypeRendererActivationEvidenceV1 make_request(
    const BrokerRequest& request, const bool loaded_by_request) noexcept {
    MacTypeRendererActivationEvidenceV1 evidence{};
    evidence.struct_size = MACTYPE_RENDERER_ACTIVATION_EVIDENCE_V1_SIZE;
    evidence.schema_version = MACTYPE_RENDERER_ACTIVATION_SCHEMA_VERSION;
    evidence.reason = MACTYPE_RENDERER_REASON_NONE;
#if defined(_WIN64)
    evidence.architecture = MACTYPE_RENDERER_ARCHITECTURE_X64;
#else
    evidence.architecture = MACTYPE_RENDERER_ARCHITECTURE_X86;
#endif
    evidence.module_load = loaded_by_request
                               ? MACTYPE_RENDERER_MODULE_LOAD_LOADED_BY_REQUEST
                               : MACTYPE_RENDERER_MODULE_LOAD_ALREADY_LOADED;
    evidence.pid = request.pid;
    evidence.session_id = request.expected_session_id;
    evidence.creation_time = request.expected_creation_time;
    if (request.generation_id.size() == MACTYPE_RENDERER_RUNTIME_GENERATION_CHARS) {
        std::memcpy(evidence.binding.runtime_generation_id, request.generation_id.data(),
                    request.generation_id.size());
    }
    if (request.profile_digest.size() == MACTYPE_RENDERER_PROFILE_DIGEST_CHARS) {
        std::memcpy(evidence.binding.profile_digest, request.profile_digest.data(),
                    request.profile_digest.size());
    }
    return evidence;
}

[[nodiscard]] bool request_identity_matches(
    const MacTypeRendererActivationEvidenceV1& expected,
    const MacTypeRendererActivationEvidenceV1& actual) noexcept {
    return actual.pid == expected.pid && actual.session_id == expected.session_id &&
           actual.creation_time == expected.creation_time &&
           actual.architecture == expected.architecture &&
           actual.module_load == expected.module_load &&
           std::memcmp(&actual.binding, &expected.binding, sizeof(actual.binding)) == 0;
}

[[nodiscard]] QueryResult query_renderer(
    HANDLE process, const BrokerRequest& request,
    const std::filesystem::path& module_path,
    const bool loaded_by_request,
    const OperationDeadline& deadline) noexcept {
    const auto remote_base = fixed_module_base(process, module_path);
    const auto export_offset = local_export_offset(module_path);
    if (!remote_base || !export_offset) {
        return {QueryState::failed, std::nullopt,
                "renderer-evidence-export-unavailable", GetLastError()};
    }
    const auto remote_export = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        *remote_base + *export_offset);
    const auto expected = make_request(request, loaded_by_request);
    if (!MacTypeValidateRendererActivationRequestV1(&expected)) {
        return {QueryState::failed, std::nullopt,
                "renderer-evidence-request-invalid", ERROR_INVALID_DATA};
    }

    void* address = VirtualAllocEx(process, nullptr, sizeof(expected),
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (address == nullptr) {
        return {QueryState::failed, std::nullopt,
                "renderer-evidence-allocation-failed", GetLastError()};
    }
    RemoteEvidence allocation{process, address};
    SIZE_T written = 0U;
    if (!WriteProcessMemory(process, allocation.get(), &expected, sizeof(expected),
                            &written) ||
        written != sizeof(expected)) {
        const DWORD error = GetLastError();
        const bool released = allocation.release();
        return {released ? QueryState::failed : QueryState::cleanup_unknown,
                std::nullopt,
                released ? "renderer-evidence-write-failed"
                         : "renderer-evidence-write-cleanup-unknown",
                error};
    }

    const DWORD wait_budget = deadline.remaining_before_reserve(
        kEvidenceTimeoutMs, kQuietUnloadReserveMs);
    if (wait_budget == 0U) {
        const bool released = allocation.release();
        return {released ? QueryState::failed : QueryState::cleanup_unknown,
                std::nullopt,
                released ? "renderer-evidence-deadline-exhausted"
                         : "renderer-evidence-deadline-cleanup-unknown",
                WAIT_TIMEOUT};
    }
    const UniqueHandle thread{CreateRemoteThread(process, nullptr, 0U, remote_export,
                                                  allocation.get(), 0U, nullptr)};
    if (!thread) {
        const DWORD error = GetLastError();
        const bool released = allocation.release();
        return {released ? QueryState::failed : QueryState::cleanup_unknown,
                std::nullopt,
                released ? "renderer-evidence-thread-failed"
                         : "renderer-evidence-thread-cleanup-unknown",
                error};
    }
    const DWORD wait = WaitForSingleObject(thread.get(), wait_budget);
    if (wait != WAIT_OBJECT_0) {
        const DWORD error = wait == WAIT_FAILED ? GetLastError() : WAIT_TIMEOUT;
        allocation.abandon();
        return {QueryState::cleanup_unknown, std::nullopt,
                wait == WAIT_TIMEOUT ? "renderer-evidence-timeout-cleanup-unknown"
                                     : "renderer-evidence-wait-cleanup-unknown",
                error};
    }

    DWORD query_status = ERROR_GEN_FAILURE;
    if (!GetExitCodeThread(thread.get(), &query_status)) {
        const DWORD error = GetLastError();
        const bool released = allocation.release();
        return {released ? QueryState::failed : QueryState::cleanup_unknown,
                std::nullopt,
                released ? "renderer-evidence-query-rejected"
                         : "renderer-evidence-query-cleanup-unknown",
                error};
    }
    if (query_status != ERROR_SUCCESS) {
        const bool released = allocation.release();
        return {released ? QueryState::failed : QueryState::cleanup_unknown,
                std::nullopt,
                released ? "renderer-evidence-query-rejected"
                         : "renderer-evidence-query-cleanup-unknown",
                query_status};
    }

    MacTypeRendererActivationEvidenceV1 actual{};
    SIZE_T read = 0U;
    const bool read_ok = ReadProcessMemory(process, allocation.get(), &actual,
                                           sizeof(actual), &read) != FALSE &&
                         read == sizeof(actual);
    const DWORD read_error = read_ok ? ERROR_SUCCESS : GetLastError();
    const bool released = allocation.release();
    const DWORD cleanup_error = released ? ERROR_SUCCESS : GetLastError();
    if (!read_ok || !released) {
        return {released ? QueryState::failed : QueryState::cleanup_unknown,
                std::nullopt,
                released ? "renderer-evidence-read-failed"
                         : "renderer-evidence-read-cleanup-unknown",
                read_error != ERROR_SUCCESS ? read_error : cleanup_error};
    }
    if (!request_identity_matches(expected, actual) ||
        !MacTypeValidateRendererActivationEvidenceV1(&actual)) {
        return {QueryState::failed, std::nullopt,
                "renderer-evidence-invalid", ERROR_INVALID_DATA};
    }
    return {QueryState::complete, actual, "renderer-evidence-verified",
            ERROR_SUCCESS};
}

[[nodiscard]] ReferenceReleaseResult release_renderer_reference(
    HANDLE process, const std::filesystem::path& module_path,
    const OperationDeadline& deadline) noexcept {
    const auto module = fixed_module_base(process, module_path);
    if (!module) {
        return {ReferenceReleaseState::cleanup_unknown,
                FixedModuleState::InventoryUnavailable, GetLastError()};
    }
    const auto free_library = remote_free_library(process);
    if (!free_library) {
        return {ReferenceReleaseState::cleanup_unknown,
                FixedModuleState::InventoryUnavailable, GetLastError()};
    }
    const DWORD wait_budget = deadline.remaining(kEvidenceTimeoutMs);
    if (wait_budget == 0U) {
        return {ReferenceReleaseState::cleanup_unknown,
                FixedModuleState::InventoryUnavailable, WAIT_TIMEOUT};
    }
    const UniqueHandle thread{CreateRemoteThread(
        process, nullptr, 0U, *free_library,
        reinterpret_cast<void*>(*module), 0U, nullptr)};
    if (!thread) {
        return {ReferenceReleaseState::cleanup_unknown,
                FixedModuleState::InventoryUnavailable, GetLastError()};
    }
    const DWORD wait = WaitForSingleObject(thread.get(), wait_budget);
    if (wait != WAIT_OBJECT_0) {
        return {ReferenceReleaseState::cleanup_unknown,
                FixedModuleState::InventoryUnavailable,
                wait == WAIT_FAILED ? GetLastError() : WAIT_TIMEOUT};
    }
    DWORD unloaded = FALSE;
    if (!GetExitCodeThread(thread.get(), &unloaded)) {
        return {ReferenceReleaseState::cleanup_unknown,
                FixedModuleState::InventoryUnavailable, GetLastError()};
    }
    if (unloaded == FALSE) {
        return {ReferenceReleaseState::cleanup_unknown,
                FixedModuleState::InventoryUnavailable, ERROR_DLL_INIT_FAILED};
    }
    const FixedModuleState state = fixed_module_state(process, module_path);
    DWORD state_error = ERROR_SUCCESS;
    if (state == FixedModuleState::InventoryUnavailable) {
        state_error = GetLastError();
        if (state_error == ERROR_SUCCESS) {
            state_error = ERROR_PARTIAL_COPY;
        }
    }
    return {ReferenceReleaseState::complete, state, state_error};
}

[[nodiscard]] bool expected_post_release_state(
    const RendererLoadOrigin load_origin,
    const MacTypeRendererActivationDisposition disposition,
    const FixedModuleState state) noexcept {
    if (load_origin == RendererLoadOrigin::loaded_by_request) {
        return disposition == MACTYPE_RENDERER_DISPOSITION_QUIET_SKIP &&
               state == FixedModuleState::Absent;
    }
    if (disposition == MACTYPE_RENDERER_DISPOSITION_ACTIVE) {
        return state == FixedModuleState::ExpectedModuleLoaded;
    }
    return state == FixedModuleState::ExpectedModuleLoaded ||
           state == FixedModuleState::Absent;
}

void record_release_uncertainty(
    Result& result, const ReferenceReleaseResult& release,
    const std::string_view code =
        "renderer-reference-release-cleanup-unknown") noexcept {
    result.status = ResultStatus::timed_out;
    result.code = code;
    result.windows_error = release.windows_error;
    result.cleanup_complete = false;
}

}  // namespace

Result bind_renderer_activation_evidence(
    HANDLE process, const BrokerRequest& request,
    const std::filesystem::path& module_path, Result module_result,
    const RendererLoadOrigin load_origin,
    const OperationDeadline& deadline) noexcept {
    if (module_result.status != ResultStatus::injected) {
        return module_result;
    }
    const QueryResult query = query_renderer(
        process, request, module_path,
        load_origin == RendererLoadOrigin::loaded_by_request, deadline);
    if (query.state != QueryState::complete || !query.evidence) {
        if (load_origin == RendererLoadOrigin::already_loaded &&
            query.state == QueryState::failed) {
            const ReferenceReleaseResult release =
                release_renderer_reference(process, module_path, deadline);
            if (release.state != ReferenceReleaseState::complete) {
                record_release_uncertainty(module_result, release);
                return module_result;
            }
        }
        module_result.status = query.state == QueryState::cleanup_unknown
                                   ? ResultStatus::timed_out
                                   : ResultStatus::integrity_failed;
        module_result.code = query.code;
        module_result.windows_error = query.windows_error;
        module_result.cleanup_complete =
            load_origin == RendererLoadOrigin::already_loaded &&
            query.state == QueryState::failed;
        return module_result;
    }

    module_result.renderer_evidence = query.evidence;
    const auto disposition = query.evidence->disposition;
    if (load_origin == RendererLoadOrigin::already_loaded ||
        disposition == MACTYPE_RENDERER_DISPOSITION_QUIET_SKIP) {
        const ReferenceReleaseResult release =
            release_renderer_reference(process, module_path, deadline);
        if (release.state != ReferenceReleaseState::complete) {
            record_release_uncertainty(
                module_result, release,
                load_origin == RendererLoadOrigin::loaded_by_request
                    ? "renderer-quiet-skip-unload-cleanup-unknown"
                    : "renderer-reference-release-cleanup-unknown");
            return module_result;
        }
        if (!expected_post_release_state(
                load_origin, disposition, release.module_state)) {
            module_result.renderer_evidence.reset();
            module_result.status = ResultStatus::integrity_failed;
            module_result.code = "renderer-reference-release-state-invalid";
            module_result.windows_error = release.windows_error != ERROR_SUCCESS
                                              ? release.windows_error
                                              : ERROR_INVALID_DATA;
            module_result.cleanup_complete = true;
            return module_result;
        }
    }
    if (disposition == MACTYPE_RENDERER_DISPOSITION_ACTIVE) {
        module_result.status = ResultStatus::injected;
        module_result.code = "renderer-active";
        module_result.windows_error = ERROR_SUCCESS;
        module_result.cleanup_complete = true;
        return module_result;
    }
    if (disposition == MACTYPE_RENDERER_DISPOSITION_QUIET_SKIP) {
        module_result.status = ResultStatus::skipped;
        module_result.code = MacTypeRendererActivationReasonCode(query.evidence->reason);
        module_result.windows_error = ERROR_SUCCESS;
        module_result.cleanup_complete = true;
        return module_result;
    }

    module_result.status = ResultStatus::failed;
    module_result.code = MacTypeRendererActivationReasonCode(query.evidence->reason);
    module_result.windows_error = ERROR_SUCCESS;
    module_result.cleanup_complete = load_origin == RendererLoadOrigin::already_loaded;
    return module_result;
}

}  // namespace mactype::injector
