#include "child_process_relay.h"

#include "detours.h"
#include "renderer_raii.h"
#include "../shared/hook_compatibility.h"

#include <cstdio>
#include <string>
#include <vector>

BOOL IsExeUnload(LPCTSTR executableName);

namespace {

constexpr DWORD kUnsupportedCreationFlags =
    DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | DETACHED_PROCESS |
    CREATE_PROTECTED_PROCESS | CREATE_SECURE_PROCESS;
constexpr DWORD kHelperTimeoutMilliseconds = 5'000;
constexpr DWORD kRelayHelperQuietSkipBase = 0x2000'0000;
constexpr WCHAR kRelayMarker[] = L"--mactype-relay-helper";

thread_local bool g_childRelayBypass = false;

class ScopedChildRelayBypass
{
public:
    ScopedChildRelayBypass() noexcept : previous_(g_childRelayBypass)
    {
        g_childRelayBypass = true;
    }
    ~ScopedChildRelayBypass() { g_childRelayBypass = previous_; }

    ScopedChildRelayBypass(ScopedChildRelayBypass const&) = delete;
    ScopedChildRelayBypass& operator=(ScopedChildRelayBypass const&) = delete;

private:
    bool previous_;
};

class ProcessAttributeList
{
public:
    ProcessAttributeList() = default;
    ~ProcessAttributeList()
    {
        if (initialized_)
            DeleteProcThreadAttributeList(get());
    }

    ProcessAttributeList(ProcessAttributeList const&) = delete;
    ProcessAttributeList& operator=(ProcessAttributeList const&) = delete;

    bool Initialize(HANDLE const* handles, size_t count)
    {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0)
            return false;
        storage_.resize((bytes + sizeof(ULONG_PTR) - 1) / sizeof(ULONG_PTR));
        if (!InitializeProcThreadAttributeList(get(), 1, 0, &bytes))
            return false;
        initialized_ = true;
        return UpdateProcThreadAttribute(
                   get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                   const_cast<HANDLE*>(handles), sizeof(HANDLE) * count,
                   nullptr, nullptr) != FALSE;
    }

    LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept
    {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    }

private:
    std::vector<ULONG_PTR> storage_;
    bool initialized_ = false;
};

struct VerifiedChild final
{
    HANDLE process = nullptr;
    HANDLE primaryThread = nullptr;
    DWORD processId = 0;
    unsigned long long creationTime = 0;
    DWORD sessionId = 0;
    bool target32Bit = false;
};

struct InjectionAttempt final
{
    ChildRelayDisposition disposition;
    ChildRelayReason reason;
    DWORD windowsError;
};

InjectionAttempt Injected() noexcept
{
    return {ChildRelayDisposition::injected, ChildRelayReason::none, ERROR_SUCCESS};
}

InjectionAttempt QuietSkip(ChildRelayReason reason) noexcept
{
    return {ChildRelayDisposition::quietlySkipped, reason, ERROR_SUCCESS};
}

InjectionAttempt RelayFailure(
    ChildRelayReason reason, DWORD error = ERROR_GEN_FAILURE) noexcept
{
    return {ChildRelayDisposition::relayFailed, reason, error};
}

InjectionAttempt UnsafeToResume(
    ChildRelayReason reason, DWORD error) noexcept
{
    return {ChildRelayDisposition::unsafeToResume, reason, error};
}

DWORD EncodeQuietSkip(ChildRelayReason reason) noexcept
{
    return kRelayHelperQuietSkipBase + static_cast<DWORD>(reason);
}

bool DecodeQuietSkip(DWORD exitCode, ChildRelayReason& reason) noexcept
{
    if (exitCode < kRelayHelperQuietSkipBase)
        return false;
    DWORD const value = exitCode - kRelayHelperQuietSkipBase;
    if (value > static_cast<DWORD>(ChildRelayReason::resumeFailed))
        return false;
    reason = static_cast<ChildRelayReason>(value);
    return reason != ChildRelayReason::none;
}

bool TryGetTarget32Bit(HANDLE process, bool& target32Bit) noexcept
{
    using IsWow64Process2Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    HMODULE const kernel = GetModuleHandleW(L"kernel32.dll");
    auto const isWow64Process2 = kernel == nullptr ? nullptr :
        reinterpret_cast<IsWow64Process2Function>(
            GetProcAddress(kernel, "IsWow64Process2"));
    if (isWow64Process2 != nullptr)
    {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process, &processMachine, &nativeMachine))
            return false;
        if (processMachine == IMAGE_FILE_MACHINE_I386 ||
            (processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
             nativeMachine == IMAGE_FILE_MACHINE_I386))
        {
            target32Bit = true;
            return true;
        }
        if (processMachine == IMAGE_FILE_MACHINE_AMD64 ||
            (processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
             nativeMachine == IMAGE_FILE_MACHINE_AMD64))
        {
            target32Bit = false;
            return true;
        }
        return false;
    }

    BOOL currentWow64 = FALSE;
    BOOL targetWow64 = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &currentWow64) ||
        !IsWow64Process(process, &targetWow64))
        return false;
#ifdef _WIN64
    target32Bit = targetWow64 != FALSE;
#else
    target32Bit = currentWow64 != FALSE ? targetWow64 != FALSE : true;
#endif
    return true;
}

mactype::hooking::HookCompatibility QueryHookCompatibility(
    HANDLE process) noexcept
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    bool const dynamicCodeKnown = GetProcessMitigationPolicy(
        process, ProcessDynamicCodePolicy, &dynamicCode,
        sizeof(dynamicCode)) != FALSE;
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
    bool const signatureKnown = GetProcessMitigationPolicy(
        process, ProcessSignaturePolicy, &signature,
        sizeof(signature)) != FALSE;
    return mactype::hooking::ClassifyHookCompatibility({
        dynamicCodeKnown,
        dynamicCodeKnown && dynamicCode.ProhibitDynamicCode != 0,
        dynamicCodeKnown && dynamicCode.AllowThreadOptOut != 0,
        signatureKnown,
        signatureKnown && signature.MicrosoftSignedOnly != 0,
        signatureKnown && signature.StoreSignedOnly != 0,
        signatureKnown && signature.MitigationOptIn != 0,
    });
}

bool IsInnoUninstaller(WCHAR const* name) noexcept
{
    if (name == nullptr)
        return false;
    WCHAR const* const extension = wcsrchr(name, L'.');
    if (extension == nullptr ||
        (_wcsicmp(extension, L".exe") != 0 && _wcsicmp(extension, L".tmp") != 0))
        return false;
    WCHAR const* stem = name;
    if (*stem == L'_')
        ++stem;
    if (_wcsnicmp(stem, L"unins", 5) != 0)
        return false;
    stem += 5;
    if (stem == extension)
        return true;
    for (; stem != extension; ++stem)
    {
        if (*stem < L'0' || *stem > L'9')
            return false;
    }
    return true;
}

bool IsPolicyExcludedImage(WCHAR const* name) noexcept
{
    if (name == nullptr || *name == L'\0')
        return false;
    WCHAR const* const exactNames[] = {
        L"smss.exe",
        L"csrss.exe",
        L"wininit.exe",
        L"winlogon.exe",
        L"services.exe",
        L"lsass.exe",
        L"fontdrvhost.exe",
        L"mactype-service.exe",
        L"mactype-injector32.exe",
        L"mactype-injector64.exe",
        L"mactype-service-setup.exe",
    };
    for (WCHAR const* const excluded : exactNames)
    {
        if (_wcsicmp(name, excluded) == 0)
            return true;
    }
    return IsInnoUninstaller(name);
}

ChildRelayReason ClassifyTarget(
    HANDLE process, DWORD sessionId, bool applyProfileExclusions) noexcept
{
    if (sessionId == 0)
        return ChildRelayReason::sessionZero;

    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    if (!GetProcessInformation(
            process, ProcessProtectionLevelInfo, &protection,
            sizeof(protection)))
        return ChildRelayReason::protectionUnavailable;
    if (protection.ProtectionLevel != PROTECTION_LEVEL_NONE)
        return ChildRelayReason::protectedProcess;

    BOOL critical = FALSE;
    if (!IsProcessCritical(process, &critical))
        return ChildRelayReason::criticalityUnavailable;
    if (critical)
        return ChildRelayReason::criticalProcess;

    try
    {
        std::vector<WCHAR> image(32'768, L'\0');
        DWORD length = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(
                process, 0, image.data(), &length) ||
            length == 0 || length >= static_cast<DWORD>(image.size()))
            return ChildRelayReason::imageNameUnavailable;
        image[static_cast<size_t>(length)] = L'\0';
        WCHAR const* const slash = wcsrchr(image.data(), L'\\');
        WCHAR const* const name = slash == nullptr ? image.data() : slash + 1;
        if (*name == L'\0')
            return ChildRelayReason::imageNameUnavailable;
        if (IsPolicyExcludedImage(name) ||
            (applyProfileExclusions && IsExeUnload(name) != FALSE))
            return ChildRelayReason::excludedImage;
    }
    catch (...)
    {
        return ChildRelayReason::imageNameUnavailable;
    }

    switch (QueryHookCompatibility(process))
    {
    case mactype::hooking::HookCompatibility::compatible:
        return ChildRelayReason::none;
    case mactype::hooking::HookCompatibility::dynamic_code_prohibited:
        return ChildRelayReason::dynamicCodeProhibited;
    case mactype::hooking::HookCompatibility::binary_signature_restricted:
        return ChildRelayReason::binarySignatureRestricted;
    }
    return ChildRelayReason::none;
}

bool CurrentModuleDirectory(std::wstring& directory, bool& coreBuild) noexcept
{
    try
    {
        std::vector<WCHAR> current(32'768, L'\0');
        DWORD const length = GetModuleFileNameW(
            GetDLLInstance(), current.data(), static_cast<DWORD>(current.size()));
        if (length == 0 || length >= static_cast<DWORD>(current.size()))
            return false;
        current[static_cast<size_t>(length)] = L'\0';
        WCHAR* const slash = wcsrchr(current.data(), L'\\');
        if (slash == nullptr || slash[1] == L'\0')
            return false;
        std::wstring const currentName(slash + 1);
        coreBuild =
            _wcsicmp(currentName.c_str(), L"MacType.Core.dll") == 0 ||
            _wcsicmp(currentName.c_str(), L"MacType64.Core.dll") == 0;
        slash[1] = L'\0';
        directory.assign(current.data());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool FixedModulePath(bool target32Bit, std::wstring& target) noexcept
{
    std::wstring directory;
    bool coreBuild = false;
    if (!CurrentModuleDirectory(directory, coreBuild))
        return false;
    try
    {
        std::wstring const profile = directory + L"MacType.ini";
        DWORD const profileAttributes = GetFileAttributesW(profile.c_str());
        if (profileAttributes == INVALID_FILE_ATTRIBUTES ||
            (profileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return false;
        WCHAR const* const targetName = target32Bit
            ? (coreBuild ? L"MacType.Core.dll" : L"MacType.dll")
            : (coreBuild ? L"MacType64.Core.dll" : L"MacType64.dll");
        target = directory;
        target.append(targetName);
        DWORD const attributes = GetFileAttributesW(target.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    catch (...)
    {
        return false;
    }
}

bool TargetIdentity(
    HANDLE process,
    DWORD processId,
    unsigned long long& creationTime,
    DWORD& sessionId) noexcept
{
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(
            process, &created, &exited, &kernel, &user) ||
        !ProcessIdToSessionId(processId, &sessionId))
        return false;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    creationTime = value.QuadPart;
    return creationTime != 0;
}

bool CaptureVerifiedChild(
    PROCESS_INFORMATION const& process, VerifiedChild& target) noexcept
{
    if (process.hProcess == nullptr || process.hThread == nullptr ||
        process.dwProcessId == 0 || GetProcessId(process.hProcess) != process.dwProcessId)
        return false;
    target.process = process.hProcess;
    target.primaryThread = process.hThread;
    target.processId = process.dwProcessId;
    if (!TargetIdentity(
            target.process, target.processId,
            target.creationTime, target.sessionId) ||
        !TryGetTarget32Bit(target.process, target.target32Bit))
        return false;
    return true;
}

bool RevalidateVerifiedChild(VerifiedChild const& target) noexcept
{
    if (GetProcessId(target.process) != target.processId)
        return false;
    unsigned long long creationTime = 0;
    DWORD sessionId = 0;
    bool target32Bit = false;
    return TargetIdentity(
               target.process, target.processId, creationTime, sessionId) &&
        creationTime == target.creationTime && sessionId == target.sessionId &&
        TryGetTarget32Bit(target.process, target32Bit) &&
        target32Bit == target.target32Bit;
}

bool OppositeArchitectureRundll32(std::wstring& path) noexcept
{
    try
    {
        std::vector<WCHAR> windowsDirectory(32'768, L'\0');
        UINT const length = GetWindowsDirectoryW(
            windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
        if (length == 0 ||
            length >= static_cast<UINT>(windowsDirectory.size()))
            return false;
        path.assign(windowsDirectory.data(), length);
#ifdef _WIN64
        path.append(L"\\SysWOW64\\rundll32.exe");
#else
        path.append(L"\\Sysnative\\rundll32.exe");
#endif
        DWORD const attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    catch (...)
    {
        return false;
    }
}

InjectionAttempt InjectWithMatchingHelper(
    VerifiedChild const& target) noexcept
{
    try
    {
        std::wstring modulePath;
        std::wstring helperPath;
        if (!FixedModulePath(target.target32Bit, modulePath) ||
            !OppositeArchitectureRundll32(helperPath))
            return RelayFailure(
                ChildRelayReason::fixedGenerationUnavailable,
                ERROR_FILE_NOT_FOUND);

        HANDLE inheritedTargetRaw = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(), target.process, GetCurrentProcess(),
                &inheritedTargetRaw, 0, TRUE, DUPLICATE_SAME_ACCESS))
            return RelayFailure(
                ChildRelayReason::mixedArchitectureHelperFailed,
                GetLastError());
        auto inheritedTarget = renderer_raii::AdoptHandle(inheritedTargetRaw);
        HANDLE const inherited[] = {inheritedTarget.get()};
        ProcessAttributeList attributes;
        if (!attributes.Initialize(inherited, _countof(inherited)))
            return RelayFailure(
                ChildRelayReason::mixedArchitectureHelperFailed,
                GetLastError());
        std::wstring command = L"\"" + helperPath + L"\" \"" +
            modulePath + L"\",RelayChildProcess " + kRelayMarker + L" " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(inheritedTarget.get())) +
            L" " + std::to_wstring(target.processId) +
            L" " + std::to_wstring(target.creationTime) +
            L" " + std::to_wstring(target.sessionId);
        std::vector<WCHAR> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes.get();
        PROCESS_INFORMATION helper{};
        {
            ScopedChildRelayBypass bypass;
            if (!CreateProcessW(
                    helperPath.c_str(), mutableCommand.data(), nullptr, nullptr,
                    TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                    nullptr, nullptr, &startup.StartupInfo, &helper))
                return RelayFailure(
                    ChildRelayReason::mixedArchitectureHelperFailed,
                    GetLastError());
        }
        auto helperProcess = renderer_raii::AdoptHandle(helper.hProcess);
        auto helperThread = renderer_raii::AdoptHandle(helper.hThread);
        DWORD const wait = WaitForSingleObject(
            helperProcess.get(), kHelperTimeoutMilliseconds);
        if (wait != WAIT_OBJECT_0)
        {
            DWORD const error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            TerminateProcess(helperProcess.get(), error);
            WaitForSingleObject(helperProcess.get(), kHelperTimeoutMilliseconds);
            return UnsafeToResume(
                wait == WAIT_TIMEOUT
                    ? ChildRelayReason::mixedArchitectureHelperTimeout
                    : ChildRelayReason::mixedArchitectureHelperFailed,
                error);
        }
        DWORD exitCode = 0;
        bool const hasExitCode = GetExitCodeProcess(helperProcess.get(), &exitCode) != FALSE;
        if (!hasExitCode)
            return RelayFailure(
                ChildRelayReason::mixedArchitectureHelperFailed,
                GetLastError());
        if (exitCode == ERROR_SUCCESS)
            return Injected();
        ChildRelayReason quietReason = ChildRelayReason::none;
        if (DecodeQuietSkip(exitCode, quietReason))
            return QuietSkip(quietReason);
        return RelayFailure(
            ChildRelayReason::mixedArchitectureHelperFailed,
            exitCode);
    }
    catch (...)
    {
        return RelayFailure(
            ChildRelayReason::mixedArchitectureHelperFailed,
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

bool AnsiModulePath(std::wstring const& wide, std::string& ansi) noexcept
{
    BOOL usedDefault = FALSE;
    int const length = WideCharToMultiByte(
        CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1, nullptr, 0,
        nullptr, &usedDefault);
    if (length <= 1 || usedDefault)
        return false;
    ansi.assign(static_cast<size_t>(length), '\0');
    usedDefault = FALSE;
    if (WideCharToMultiByte(
            CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1,
            &ansi[0], length, nullptr, &usedDefault) != length || usedDefault)
        return false;
    ansi.resize(static_cast<size_t>(length - 1));
    return true;
}

InjectionAttempt InjectFixedModule(VerifiedChild const& process) noexcept
{
    try
    {
        ChildRelayReason const classification = ClassifyTarget(
            process.process, process.sessionId, true);
        if (classification != ChildRelayReason::none)
            return QuietSkip(classification);
        if (!RevalidateVerifiedChild(process))
            return RelayFailure(
                ChildRelayReason::identityUnavailable,
                ERROR_INVALID_PARAMETER);

        std::wstring module;
        if (!FixedModulePath(process.target32Bit, module))
            return RelayFailure(
                ChildRelayReason::fixedGenerationUnavailable,
                ERROR_FILE_NOT_FOUND);
        std::string ansi;
        if (!AnsiModulePath(module, ansi))
            return RelayFailure(
                ChildRelayReason::fixedGenerationUnavailable,
                ERROR_NO_UNICODE_TRANSLATION);
        bool const current32Bit = sizeof(void*) == 4;
        if (process.target32Bit != current32Bit)
            return InjectWithMatchingHelper(process);
        LPCSTR modules[] = {ansi.c_str()};
        if (DetourUpdateProcessWithDll(process.process, modules, 1) != FALSE)
            return Injected();
        return RelayFailure(
            ChildRelayReason::sameArchitectureInjectionFailed,
            GetLastError());
    }
    catch (...)
    {
        return RelayFailure(
            ChildRelayReason::sameArchitectureInjectionFailed,
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

class SuspendedChildObligation final
{
public:
    SuspendedChildObligation(
        LPPROCESS_INFORMATION process, bool callerRequestedSuspended) noexcept
        : process_(process), callerRequestedSuspended_(callerRequestedSuspended)
    {
    }

    ~SuspendedChildObligation()
    {
        if (!completed_ && process_ != nullptr && process_->hProcess != nullptr)
            TerminateAndRelease(ERROR_PROCESS_ABORTED);
    }

    void Complete(
        InjectionAttempt const& attempt,
        DWORD creationError,
        ChildInjectionTransactionResult& result) noexcept
    {
        result.relay = attempt.disposition;
        result.reason = attempt.reason;
        result.relayError = attempt.windowsError;
        if (attempt.disposition == ChildRelayDisposition::unsafeToResume)
        {
            DWORD const error = attempt.windowsError == ERROR_SUCCESS
                ? ERROR_PROCESS_ABORTED
                : attempt.windowsError;
            TerminateAndRelease(error);
            result.createResult = FALSE;
            result.lastError = error;
            result.thread = ChildThreadCompletion::terminated;
            completed_ = true;
            return;
        }
        if (callerRequestedSuspended_)
        {
            result.createResult = TRUE;
            result.lastError = creationError;
            result.thread = ChildThreadCompletion::callerSuspended;
            completed_ = true;
            return;
        }
        if (ResumeThread(process_->hThread) == static_cast<DWORD>(-1))
        {
            DWORD const error = GetLastError();
            TerminateAndRelease(error);
            result.createResult = FALSE;
            result.lastError = error;
            result.relay = ChildRelayDisposition::unsafeToResume;
            result.reason = ChildRelayReason::resumeFailed;
            result.thread = ChildThreadCompletion::terminated;
            completed_ = true;
            return;
        }
        result.createResult = TRUE;
        result.lastError = creationError;
        result.thread = ChildThreadCompletion::resumed;
        completed_ = true;
    }

private:
    void TerminateAndRelease(DWORD error) noexcept
    {
        if (process_->hProcess != nullptr)
        {
            TerminateProcess(process_->hProcess, error);
            WaitForSingleObject(process_->hProcess, kHelperTimeoutMilliseconds);
        }
        if (process_->hThread != nullptr)
            CloseHandle(process_->hThread);
        if (process_->hProcess != nullptr)
            CloseHandle(process_->hProcess);
        *process_ = PROCESS_INFORMATION{};
    }

    LPPROCESS_INFORMATION process_;
    bool callerRequestedSuspended_;
    bool completed_ = false;
};

} // namespace

bool IsChildProcessRelayHelper() noexcept
{
    WCHAR const* const commandLine = GetCommandLineW();
    if (commandLine == nullptr || wcsstr(commandLine, kRelayMarker) == nullptr)
        return false;
    WCHAR image[MAX_PATH]{};
    DWORD const length = GetModuleFileNameW(nullptr, image, _countof(image));
    if (length == 0 || length >= _countof(image))
        return false;
    WCHAR const* const slash = wcsrchr(image, L'\\');
    WCHAR const* const name = slash == nullptr ? image : slash + 1;
    return _wcsicmp(name, L"rundll32.exe") == 0;
}

EXTERN_C VOID CALLBACK RelayChildProcess(
    HWND, HINSTANCE, LPSTR commandLine, INT)
{
    unsigned long long inheritedValue = 0;
    unsigned long processIdValue = 0;
    unsigned long long creationTimeValue = 0;
    unsigned long sessionIdValue = 0;
    if (commandLine == nullptr ||
        sscanf_s(
            commandLine,
            "--mactype-relay-helper %llu %lu %llu %lu",
            &inheritedValue, &processIdValue, &creationTimeValue,
            &sessionIdValue) != 4 || inheritedValue == 0 ||
        inheritedValue > static_cast<unsigned long long>(MAXULONG_PTR) ||
        processIdValue == 0)
        ExitProcess(ERROR_INVALID_PARAMETER);

    HANDLE const process = reinterpret_cast<HANDLE>(
        static_cast<ULONG_PTR>(inheritedValue));
    VerifiedChild target{};
    target.process = process;
    target.processId = processIdValue;
    target.creationTime = creationTimeValue;
    target.sessionId = sessionIdValue;
    bool const current32Bit = sizeof(void*) == 4;
    bool valid = TryGetTarget32Bit(process, target.target32Bit) &&
        target.target32Bit == current32Bit &&
        RevalidateVerifiedChild(target);

    ChildRelayReason const classification = valid
        ? ClassifyTarget(process, target.sessionId, false)
        : ChildRelayReason::identityUnavailable;
    if (classification != ChildRelayReason::none)
    {
        ExitProcess(
            classification == ChildRelayReason::identityUnavailable
                ? ERROR_INVALID_PARAMETER
                : EncodeQuietSkip(classification));
    }

    std::wstring module;
    std::string ansi;
    valid = FixedModulePath(current32Bit, module) &&
        AnsiModulePath(module, ansi);
    BOOL injected = FALSE;
    if (valid)
    {
        LPCSTR modules[] = {ansi.c_str()};
        injected = DetourUpdateProcessWithDll(process, modules, 1);
    }
    ExitProcess(injected ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED);
}

ChildInjectionTransactionResult ExecuteVerifiedChildInjection(
    ChildProcessCreateCall const& call,
    CreateProcessInternalWFunction createProcess) noexcept
{
    CThreadCounter relayLease;
    ChildInjectionTransactionResult result{};
    if (createProcess == nullptr)
    {
        result.lastError = ERROR_PROC_NOT_FOUND;
        return result;
    }
    bool const unsupportedCreationMode =
        (call.creationFlags & kUnsupportedCreationFlags) != 0;
    if (g_childRelayBypass || call.processInformation == nullptr ||
        unsupportedCreationMode)
    {
        result.createResult = createProcess(
            call.token, call.applicationName, call.commandLine,
            call.processAttributes, call.threadAttributes, call.inheritHandles,
            call.creationFlags, call.environment, call.currentDirectory,
            call.startupInfo, call.processInformation, call.newToken);
        result.lastError = GetLastError();
        result.reason = unsupportedCreationMode
            ? ChildRelayReason::unsupportedCreationMode
            : ChildRelayReason::none;
        return result;
    }

    BOOL const created = createProcess(
        call.token, call.applicationName, call.commandLine,
        call.processAttributes, call.threadAttributes, call.inheritHandles,
        call.creationFlags | CREATE_SUSPENDED, call.environment,
        call.currentDirectory, call.startupInfo, call.processInformation,
        call.newToken);
    DWORD const creationError = GetLastError();
    if (!created)
    {
        result.lastError = creationError;
        return result;
    }

    SuspendedChildObligation obligation(
        call.processInformation,
        (call.creationFlags & CREATE_SUSPENDED) != 0);
    VerifiedChild target{};
    InjectionAttempt attempt = CaptureVerifiedChild(
        *call.processInformation, target)
        ? InjectFixedModule(target)
        : RelayFailure(
            ChildRelayReason::identityUnavailable,
            ERROR_INVALID_PARAMETER);
    obligation.Complete(attempt, creationError, result);
    return result;
}
