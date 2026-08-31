#pragma once

#include "common.h"

using CreateProcessInternalWFunction = BOOL(WINAPI*)(
    HANDLE,
    LPCTSTR,
    LPTSTR,
    LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES,
    BOOL,
    DWORD,
    LPVOID,
    LPCTSTR,
    LPSTARTUPINFO,
    LPPROCESS_INFORMATION,
    PHANDLE);

struct ChildProcessCreateCall final
{
    HANDLE token;
    LPCTSTR applicationName;
    LPTSTR commandLine;
    LPSECURITY_ATTRIBUTES processAttributes;
    LPSECURITY_ATTRIBUTES threadAttributes;
    BOOL inheritHandles;
    DWORD creationFlags;
    LPVOID environment;
    LPCTSTR currentDirectory;
    LPSTARTUPINFO startupInfo;
    LPPROCESS_INFORMATION processInformation;
    PHANDLE newToken;
};

enum class ChildRelayDisposition : unsigned char
{
    bypassed,
    injected,
    quietlySkipped,
    relayFailed,
    unsafeToResume,
};

enum class ChildRelayReason : unsigned char
{
    none,
    unsupportedCreationMode,
    identityUnavailable,
    sessionZero,
    protectionUnavailable,
    protectedProcess,
    criticalityUnavailable,
    criticalProcess,
    imageNameUnavailable,
    excludedImage,
    unsupportedArchitecture,
    dynamicCodeProhibited,
    binarySignatureRestricted,
    privateFreeTypeDetected,
    fixedGenerationUnavailable,
    sameArchitectureInjectionFailed,
    mixedArchitectureHelperFailed,
    mixedArchitectureHelperTimeout,
    resumeFailed,
};

enum class ChildThreadCompletion : unsigned char
{
    passThrough,
    callerSuspended,
    resumed,
    terminated,
};

struct ChildInjectionTransactionResult final
{
    BOOL createResult = FALSE;
    DWORD lastError = ERROR_SUCCESS;
    DWORD relayError = ERROR_SUCCESS;
    ChildRelayDisposition relay = ChildRelayDisposition::bypassed;
    ChildRelayReason reason = ChildRelayReason::none;
    ChildThreadCompletion thread = ChildThreadCompletion::passThrough;

    BOOL ReturnToHookCaller() const noexcept
    {
        SetLastError(lastError);
        return createResult;
    }
};

bool IsChildProcessRelayHelper() noexcept;
EXTERN_C VOID CALLBACK RelayChildProcess(HWND, HINSTANCE, LPSTR, INT);

ChildInjectionTransactionResult ExecuteVerifiedChildInjection(
    ChildProcessCreateCall const& call,
    CreateProcessInternalWFunction createProcess) noexcept;
