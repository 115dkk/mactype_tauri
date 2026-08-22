#include <windows.h>

#include "renderer_activation_contract.h"

#include <cstring>

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:MacTypeQueryActivationEvidenceV1=_MacTypeQueryActivationEvidenceV1@4")
#endif

extern "C" __declspec(dllexport) DWORD WINAPI MacTypeQueryActivationEvidenceV1(
    void* parameter) {
    auto* evidence = static_cast<MacTypeRendererActivationEvidenceV1*>(parameter);
    if (!MacTypeValidateRendererActivationRequestV1(evidence)) {
        return ERROR_INVALID_DATA;
    }
    const DWORD pid = GetCurrentProcessId();
    DWORD session_id = 0U;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (evidence->pid != pid || !ProcessIdToSessionId(pid, &session_id) ||
        session_id != evidence->session_id ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return ERROR_INVALID_PARAMETER;
    }
    ULARGE_INTEGER creation{};
    creation.LowPart = created.dwLowDateTime;
    creation.HighPart = created.dwHighDateTime;
    if (creation.QuadPart != evidence->creation_time) {
        return ERROR_INVALID_PARAMETER;
    }
    std::memcpy(evidence->effective_profile_digest,
                evidence->binding.profile_digest,
                sizeof(evidence->effective_profile_digest));
#ifdef MACTYPE_MARKER_QUIET_SKIP
    evidence->reason = MACTYPE_RENDERER_REASON_PROCESS_EXCLUDED;
    evidence->disposition = MACTYPE_RENDERER_DISPOSITION_QUIET_SKIP;
#else
    evidence->reason = MACTYPE_RENDERER_REASON_NONE;
    evidence->disposition = MACTYPE_RENDERER_DISPOSITION_ACTIVE;
    evidence->capability_active = MACTYPE_RENDERER_CAPABILITY_GDI;
#endif
    evidence->lifecycle_revision = 1U;
    return MacTypeValidateRendererActivationEvidenceV1(evidence)
               ? ERROR_SUCCESS
               : ERROR_INVALID_DATA;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    static_cast<void>(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
#ifdef MACTYPE_MARKER_DELAY_MS
        Sleep(MACTYPE_MARKER_DELAY_MS);
#endif
    }
    return TRUE;
}
