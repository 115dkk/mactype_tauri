// API hook
//
// GetProcAddressで得たcall先（関数本体）を直接書き換え、
// 自分のフック関数にjmpさせる。
//
// 内部で元のAPIを使う時は、コードを一度戻してからcall。
// すぐにjmpコードに戻す。
//
// マルチスレッドで 書き換え中にcallされると困るので、
// CriticalSectionで排他制御しておく。
//

#include "override.h"
#include "child_process_relay.h"
#include "ft.h"
#include "fteng.h"
#include <locale.h>
#include "undocAPI.h"
#include "delayimp.h"
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <VersionHelpers.h>
#include "EventLogging.h"
#include "hookCounter.h"
#include "hook_lifecycle.h"
#include "renderer_activation.h"
#include "unload_lifecycle.h"
#include <vector>

bool RestoreDirectWriteVtableHooks(DWORD timeoutMilliseconds = 3000);

#ifdef STATIC_LIB
#include <aux_ulib.h>
#include <psapi.h>

#pragma comment(lib, "aux_ulib.lib")
#pragma comment(lib, "psapi.lib")
#endif

#ifndef _WIN64
#include "wow64ext.h"
#endif
#ifdef INFINALITY
#include <freetype/ftenv.h>
#endif
#pragma comment(lib, "delayimp")

HINSTANCE g_dllInstance;

namespace {

class RuntimeStartGuard
{
public:
	RuntimeStartGuard()
		: coordinator_(renderer::ProcessHookCoordinator()),
		  ownsStart_(coordinator_.BeginStart())
	{
	}

	~RuntimeStartGuard()
	{
		if (ownsStart_ && !settled_)
			coordinator_.FailStart(ERROR_DLL_INIT_FAILED);
	}

	RuntimeStartGuard(const RuntimeStartGuard&) = delete;
	RuntimeStartGuard& operator=(const RuntimeStartGuard&) = delete;

	bool owns_start() const noexcept { return ownsStart_; }

	bool Complete() noexcept
	{
		if (!ownsStart_ || settled_)
			return false;
		settled_ = coordinator_.CompleteStart();
		return settled_;
	}

private:
	renderer::HookCoordinator& coordinator_;
	bool ownsStart_ = false;
	bool settled_ = false;
};

void PublishUnavailableCapability(
	renderer::HookCapability capability,
	std::uintptr_t target,
	bool modulePresent,
	renderer::CapabilityReason reason,
	LONG status = NOERROR)
{
	renderer::HookCoordinator& coordinator = renderer::ProcessHookCoordinator();
	renderer::HookAttempt const attempt = coordinator.BeginAttempt(
		capability, target, modulePresent);
	if (attempt.valid())
		coordinator.CompleteAttempt(attempt, false, reason, status);
}

LONG InstallTrackedDemandHook(
	renderer::HookCapability capability,
	void* target,
	LONG (*install)(bool))
{
	if (target == nullptr)
	{
		PublishUnavailableCapability(
			capability, 0, true,
			renderer::CapabilityReason::interfaceUnsupported,
			ERROR_PROC_NOT_FOUND);
		return ERROR_PROC_NOT_FOUND;
	}
	renderer::HookCoordinator& coordinator = renderer::ProcessHookCoordinator();
	renderer::HookAttempt const attempt = coordinator.BeginAttempt(
		capability, reinterpret_cast<std::uintptr_t>(target), true);
	if (!attempt.valid())
		return ERROR_BUSY;
	LONG const status = install(false);
	coordinator.CompleteAttempt(
		attempt, status == NOERROR,
		status == NOERROR ? renderer::CapabilityReason::none
		                  : renderer::CapabilityReason::transactionFailed,
		status);
	return status;
}

} // namespace

template <typename Target, typename Source>
void CopyHookPointer(Target& target, Source source) noexcept
{
	static_assert(sizeof(Target) == sizeof(Source), "hook pointer size mismatch");
	memcpy(&target, &source, sizeof(target));
}

//PFNLdrGetProcedureAddress LdrGetProcedureAddress = (PFNLdrGetProcedureAddress)GetProcAddress(LoadLibrary(_T("ntdll.dll")),"LdrGetProcedureAddress");
//PFNCreateProcessW nCreateProcessW = (PFNCreateProcessW)MyGetProcAddress(LoadLibrary(_T("kernel32.dll")),"CreateProcessW");
//PFNCreateProcessA nCreateProcessA = (PFNCreateProcessA)MyGetProcAddress(LoadLibrary(_T("kernel32.dll")),"CreateProcessA");
// HMODULE hGDIPP = GetModuleHandleW(L"gdiplus.dll");
// PFNGdipCreateFontFamilyFromName GdipCreateFontFamilyFromName = hGDIPP? (PFNGdipCreateFontFamilyFromName)GetProcAddress(hGDIPP, "GdipCreateFontFamilyFromName"):0;

#ifdef USE_DETOURS

#include "detours.h"
#include "detour_transaction.h"

#ifdef _M_IX86
#pragma comment (lib, "detours.lib")
#else
#pragma comment (lib, "detours64.lib")
#endif
// DATA_foo、ORIG_foo の２つをまとめて定義するマクロ
#define HOOK_MANUALLY HOOK_DEFINE
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	rettype (WINAPI * ORIG_##name) argtype; \
	BOOL IsHooked_##name = false; \
	rettype WINAPI REF_##name argtype { \
		HCounter _; \
		return IMPL_##name arglist; \
	}

#include "hooklist.h"

#undef HOOK_DEFINE
#undef HOOK_MANUALLY

//
#define HOOK_MANUALLY(rettype, name, argtype, arglist) ;
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	ORIG_##name = name;
#pragma optimize("s", on)
static void hook_initinternal()
{
#include "hooklist.h"
}
#pragma optimize("", on)
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#define HOOK_MANUALLY(rettype, name, argtype, arglist) ;
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	if (&ORIG_##name && !IsHooked_##name) { \
		if (transaction.Attach(reinterpret_cast<PVOID*>(&ORIG_##name), reinterpret_cast<PVOID>(REF_##name)) == NOERROR) IsHooked_##name = true; \
	}

static LONG hook_init()
{
	renderer::HookCoordinator& coordinator = renderer::ProcessHookCoordinator();
	renderer::HookAttempt const attempt = coordinator.BeginAttempt(
		renderer::HookCapability::gdi,
		reinterpret_cast<std::uintptr_t>(&ORIG_ExtTextOutW), true);
	if (!attempt.valid())
		return IsHooked_ExtTextOutW ? NOERROR : ERROR_BUSY;

	DetourRestoreAfterWith();

	renderer_raii::DetourTransaction transaction;

#include "hooklist.h"
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

	LONG error = transaction.Commit();

	if (error != NOERROR) {
		#define HOOK_MANUALLY HOOK_DEFINE
		#define HOOK_DEFINE(rettype, name, argtype, arglist) IsHooked_##name = false;
		#include "hooklist.h"
		#undef HOOK_DEFINE
		#undef HOOK_MANUALLY
		TRACE(_T("hook_init error: %#x\n"), error);
	}
	coordinator.CompleteAttempt(
		attempt, error == NOERROR,
		error == NOERROR ? renderer::CapabilityReason::none
		                 : renderer::CapabilityReason::transactionFailed,
		error);
	return error;
}
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#define HOOK_DEFINE(rettype, name, argtype, arglist);
#define HOOK_MANUALLY(rettype, name, argtype, arglist) \
	LONG hook_demand_##name(bool bForce = false){ \
	DetourRestoreAfterWith(); \
	renderer_raii::DetourTransaction transaction; \
	const bool shouldAttach = &ORIG_##name && (bForce || !IsHooked_##name); \
	if (shouldAttach) transaction.Attach(reinterpret_cast<PVOID*>(&ORIG_##name), reinterpret_cast<PVOID>(REF_##name)); \
	LONG error = transaction.Commit(); \
	if (error == NOERROR && shouldAttach) IsHooked_##name = true; \
	if (error != NOERROR) { \
	    TRACE(_T("hook_init error: %#x\n"), error); \
    } \
	return error; \
}

#include "hooklist.h"
#undef HOOK_MANUALLY
#undef HOOK_DEFINE

//
#define HOOK_MANUALLY HOOK_DEFINE
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	if (IsHooked_##name) transaction.Detach(reinterpret_cast<PVOID*>(&ORIG_##name), reinterpret_cast<PVOID>(REF_##name));
static void hook_term()
{
	renderer_raii::DetourTransaction transaction;

#include "hooklist.h"
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

	LONG error = transaction.Commit();

	if (error != NOERROR) {
		TRACE(_T("hook_term error: %#x\n"), error);
	} else {
		#define HOOK_MANUALLY HOOK_DEFINE
		#define HOOK_DEFINE(rettype, name, argtype, arglist) IsHooked_##name = false;
		#include "hooklist.h"
		#undef HOOK_DEFINE
		#undef HOOK_MANUALLY
	}
	HCounter::wait(3000);
}
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#else
#include "easyhook.h"
#ifdef STATIC_LIB
#ifdef _M_IX86
#pragma comment (lib, "easyhk32_s.lib")
#else
#pragma comment (lib, "easyhk64_s.lib")
#endif
#else
#ifdef _M_IX86
#pragma comment (lib, "easyhk32.lib")
#else
#pragma comment (lib, "easyhk64.lib")
#endif
#endif

#define HOOK_MANUALLY HOOK_DEFINE
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	rettype (WINAPI * ORIG_##name) argtype; \
	HOOK_TRACE_INFO HOOK_##name = {0};	//建立hook结构

#include "hooklist.h"
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

//
#define HOOK_MANUALLY(rettype, name, argtype, arglist) ;
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	ORIG_##name = name;
#pragma optimize("s", on)
static void hook_initinternal()
{
#include "hooklist.h"
}
#pragma optimize("", on)
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#define FORCE(expr) {if(!SUCCEEDED(NtStatus = (expr))) goto ERROR_ABORT;}

#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	if (&ORIG_##name) { \
	FORCE(LhInstallHook(reinterpret_cast<PVOID&>(ORIG_##name), reinterpret_cast<PVOID>(IMPL_##name), nullptr, &HOOK_##name)); \
	CopyHookPointer(ORIG_##name, HOOK_##name.Link->OldProc); \
	FORCE(LhSetExclusiveACL(ACLEntries, 0, &HOOK_##name)); }
#define HOOK_MANUALLY(rettype, name, argtype, arglist) ;

static LONG hook_init()
{
	ULONG ACLEntries[1] = {0};
	NTSTATUS NtStatus;

#include "hooklist.h"
#undef HOOK_DEFINE

	FORCE(LhSetGlobalExclusiveACL(ACLEntries, 0));
	return NOERROR;

ERROR_ABORT:
	TRACE(_T("hook_init error: %#x\n"), NtStatus);
	return 1;
}
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#define HOOK_DEFINE(rettype, name, argtype, arglist);
#define HOOK_MANUALLY(rettype, name, argtype, arglist) \
	LONG hook_demand_##name(bool bForce = false){ \
	NTSTATUS NtStatus; \
	ULONG ACLEntries[1] = { 0 }; \
	if (bForce) {  \
		memset(&HOOK_##name, 0, sizeof(HOOK_TRACE_INFO));  \
	}  \
	if (&ORIG_##name) {	\
	FORCE(LhInstallHook(reinterpret_cast<PVOID&>(ORIG_##name), reinterpret_cast<PVOID>(IMPL_##name), nullptr, &HOOK_##name)); \
	CopyHookPointer(ORIG_##name, HOOK_##name.Link->OldProc); \
	FORCE(LhSetExclusiveACL(ACLEntries, 0, &HOOK_##name)); } \
	return NOERROR; \
	ERROR_ABORT: \
	TRACE(_T("hook_init error: %#x\n"), NtStatus); \
	return 1; \
	}

#include "hooklist.h"
#undef HOOK_MANUALLY


#undef HOOK_MANUALLY
#undef HOOK_DEFINE

#define HOOK_MANUALLY(rettype, name, argtype, arglist) ;
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	ORIG_##name = name;
#pragma optimize("s", on)
static LONG hook_term()
{
	#include "hooklist.h"
	LhUninstallAllHooks();
	return LhWaitForPendingRemovals();
}
#endif
#pragma optimize("", on)
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

//---

CTlsData<CThreadLocalInfo>	g_TLInfo;
HINSTANCE					g_hinstDLL;
LONG						g_bHookEnabled;
#ifdef _DEBUG
#endif

//void InstallManagerHook();
//void RemoveManagerHook();


//ベースアドレスを変えた方がロードが早くなる
#if _DLL
#pragma comment(linker, "/base:0x06540000")
#endif

typedef BOOL(WINAPI *TIsImmersiveProcess)(_In_ HANDLE hProcess);

TIsImmersiveProcess IsUWP = reinterpret_cast<TIsImmersiveProcess>(GetProcAddress(GetModuleHandle(L"user32.dll"), "IsImmersiveProcess"));

BOOL WINAPI IsRunAsUser(VOID)
{
	if (IsUWP && IsUWP(GetCurrentProcess())) return true;	// treat all UWP apps as user exe
	renderer_raii::UniqueHandle processToken;
	DWORD groupLength = 50;

	renderer_raii::UniqueLocalMemory<TOKEN_GROUPS> groupInfo(
		static_cast<PTOKEN_GROUPS>(LocalAlloc(0, groupLength)));

	SID_IDENTIFIER_AUTHORITY siaNt = SECURITY_NT_AUTHORITY;
	renderer_raii::UniqueSid interactiveSid;
	renderer_raii::UniqueSid serviceSid;
	PSID rawInteractiveSid = nullptr;
	PSID rawServiceSid = nullptr;
	DWORD i;

	// Start with assumption that process is an SERVICE, not a EXE;
	BOOL fExe = FALSE;


	HANDLE rawProcessToken = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawProcessToken))
		goto ret;
	processToken = renderer_raii::AdoptHandle(rawProcessToken);

	if (!groupInfo)
		goto ret;

	if (!GetTokenInformation(processToken.get(), TokenGroups, groupInfo.get(),
		groupLength, &groupLength))
	{
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
			goto ret;

		groupInfo.reset(static_cast<PTOKEN_GROUPS>(LocalAlloc(0, groupLength)));

		if (!groupInfo)
			goto ret;

		if (!GetTokenInformation(processToken.get(), TokenGroups, groupInfo.get(),
			groupLength, &groupLength))
		{
			goto ret;
		}
	}

	// Interactive membership identifies a desktop process. Service membership
	// identifies a service launched in a user account by the service controller.
	if (!AllocateAndInitializeSid(&siaNt, 1, SECURITY_INTERACTIVE_RID, 0,
		0,
		0, 0, 0, 0, 0, &rawInteractiveSid))
	{
		goto ret;
	}
	interactiveSid.reset(rawInteractiveSid);

	if (!AllocateAndInitializeSid(&siaNt, 1, SECURITY_SERVICE_RID, 0, 0, 0,
		0, 0, 0, 0, &rawServiceSid))
	{
		goto ret;
	}
	serviceSid.reset(rawServiceSid);

	for (i = 0; i < groupInfo->GroupCount; i += 1)
	{
		SID_AND_ATTRIBUTES sanda = groupInfo->Groups[i];
		PSID Sid = sanda.Sid;

		if (EqualSid(Sid, interactiveSid.get()))
		{
			fExe = true;
			goto ret;
		}
		else if (EqualSid(Sid, serviceSid.get()))
		{
			fExe = FALSE;
			goto ret;
		}
	}

	// A token with neither SID is treated conservatively as a service host.
	fExe = FALSE;

ret:
	return(fExe);
}

BOOL AddEasyHookEnv()
{
	TCHAR dir[MAX_PATH];
	int dirlen = GetModuleFileName(GetDLLInstance(), dir, MAX_PATH);
	LPTSTR lpfilename=dir+dirlen;
	while (lpfilename>dir && *lpfilename!=_T('\\') && *lpfilename!=_T('/')) --lpfilename;
	*lpfilename = 0;
	_tcscat(dir, _T(";"));
	dirlen = _tcslen(dir);
	const DWORD pathLength = GetEnvironmentVariable(_T("path"), nullptr, 0);
	std::vector<TCHAR> path(static_cast<size_t>(pathLength) + dirlen + 2, _T('\0'));
	if (pathLength != 0) {
		GetEnvironmentVariable(_T("path"), path.data(), pathLength);
	}
	if (!_tcsstr(path.data(), dir))
	{
		const size_t currentLength = _tcslen(path.data());
		if (currentLength != 0 && path[currentLength - 1] != _T(';'))
			_tcscat(path.data(), _T(";"));
		_tcscat(path.data(), dir);
		SetEnvironmentVariable(_T("path"), path.data());
	}
	return true;
}

void HookFontCreation() {
	HMODULE gdi32 = GetModuleHandle(L"gdi32full.dll");	// prefer to hook deeply
	if (!gdi32) {
		gdi32 = GetModuleHandle(L"gdi32.dll");
	}
	if (gdi32) {
		void* CreateFontIndirectW = GetProcAddress(gdi32, "CreateFontIndirectWImpl");
		void* CreateFontIndirectExW = GetProcAddress(gdi32, "CreateFontIndirectExW");
		if (!CreateFontIndirectW) {
			CreateFontIndirectW = GetProcAddress(gdi32, "CreateFontIndirectW");
		}
		CopyHookPointer(ORIG_CreateFontIndirectW, CreateFontIndirectW);
		CopyHookPointer(ORIG_CreateFontIndirectExW, CreateFontIndirectExW);

		InstallTrackedDemandHook(
			renderer::HookCapability::fontSubstitution,
			CreateFontIndirectExW, hook_demand_CreateFontIndirectExW);
		InstallTrackedDemandHook(
			renderer::HookCapability::fontSubstitution,
			CreateFontIndirectW, hook_demand_CreateFontIndirectW);
	}
	else
	{
		PublishUnavailableCapability(
			renderer::HookCapability::fontSubstitution, 0, false,
			renderer::CapabilityReason::moduleMissing,
			ERROR_MOD_NOT_FOUND);
	}
}

extern FT_Int * g_charmapCache;
extern BYTE* AACache, *AACacheFull;
extern HFONT g_alterGUIFont;
extern void DebugOut(const WCHAR* szFormat, ...);


void EZHookMain(HINSTANCE instance, DWORD reason, LPVOID lpReserved) {
#ifdef STATIC_LIB
	switch (reason) {
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		EasyHookDllMain(instance, reason, lpReserved);
	}
#else
	switch (reason) {
	case DLL_PROCESS_ATTACH:
	{
		std::vector<WCHAR> dllPath(MAX_PATH + 1, L'\0');
		int nSize = GetModuleFileName(g_dllInstance, dllPath.data(), static_cast<DWORD>(dllPath.size()));
		WCHAR* p = &dllPath[nSize];
		while (*--p != L'\\');
		*p = L'\0';
#ifdef _WIN64
		wcscat(dllPath.data(), L"\\easyhk64.dll");
#else
		wcscat(dllPath.data(), L"\\easyhk32.dll");
#endif
		HMODULE hEasyhk = LoadLibrary(dllPath.data());
		if (!hEasyhk) {
			DebugOut(L"Failed to load Easyhook, exiting");
			return;
		}
	}
	}
#endif
}

extern COLORCACHE* g_AACache2[MAX_CACHE_SIZE];
HANDLE hDelayHook = 0;

BOOL WINAPI  DllMain(HINSTANCE instance, DWORD reason, LPVOID lpReserved)
{
	try {
		static bool bDllInited = false;
		BOOL IsUnload = false, bEnableDW = true, bUseFontSubstitute = false;
		bool bHookChildProcesses = false;

#ifdef USE_DETOURS
		if (IsChildProcessRelayHelper())
		{
			if (reason == DLL_PROCESS_ATTACH)
			{
				g_dllInstance = instance;
				g_hinstDLL = instance;
				DisableThreadLibraryCalls(instance);
			}
			return TRUE;
		}
#endif


		switch (reason) {
		case DLL_PROCESS_ATTACH:
		{
			DebugOut(L"Begin core loading stage, pid %d", ::GetCurrentProcessId());
			if (bDllInited)
				return true;
			RuntimeStartGuard runtimeStart;
			if (!runtimeStart.owns_start())
				return FALSE;
			g_dllInstance = instance;
#ifdef EASYHOOK
			EZHookMain(instance, reason, lpReserved);
#endif
			_CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_LEAK_CHECK_DF);
			_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_WNDW);
			g_hinstDLL = instance;

			CCriticalSectionLock::Init();
			COwnedCriticalSectionLock::Init();
			CThreadCounter::Init();
			if (!g_TLInfo.ProcessInit()) {
				DebugOut(L"Can't initialize process, exiting");
				return FALSE;
			}

			// Explicit unload is unsafe until these process-wide primitives exist.
			bDllInited = true;

			{
#ifdef INFINALITY
				// enable infinality exclusive features
				FT_initEnv();
#endif
				CGdippSettings* pSettings = CGdippSettings::CreateInstance();
				if (!pSettings || !pSettings->LoadSettings(instance)) {
					CGdippSettings::DestroyInstance();
					return FALSE;
				}
				IsUnload = IsProcessUnload();
				bEnableDW = pSettings->DirectWrite();
				bUseFontSubstitute = !!pSettings->FontSubstitutes();
				bHookChildProcesses = pSettings->HookChildProcesses();
			}
			if (!IsUnload) hook_initinternal();	//不加载的模块就不做任何事莵E
			const bool processExcluded = IsProcessExcluded();
			if (!processExcluded && !IsUnload) {
#ifndef _WIN64
				InitWow64ext();
#endif
				if (!FontLInit()) {
					DebugOut(L"FreeType failed to initialize, exiting");
					return FALSE;
				}
				if (!CreateFreeTypeFontEngine()) {
					return FALSE;
				}
				InterlockedExchange(&g_bHookEnabled, TRUE);
				if (hook_init() != NOERROR) {
					DebugOut(L"Can't do hooking, exiting");
					return FALSE;
				}
#ifdef USE_DETOURS
				if (bHookChildProcesses && CreateProcessInternalW != nullptr)
				{
					CopyHookPointer(
						ORIG_CreateProcessInternalW, CreateProcessInternalW);
					renderer::HookCoordinator& coordinator =
						renderer::ProcessHookCoordinator();
					renderer::HookAttempt const attempt = coordinator.BeginAttempt(
						renderer::HookCapability::childInjection,
						reinterpret_cast<std::uintptr_t>(&ORIG_CreateProcessInternalW),
						true);
					LONG const childHookStatus = hook_demand_CreateProcessInternalW();
					if (attempt.valid())
						coordinator.CompleteAttempt(
							attempt, childHookStatus == NOERROR,
							childHookStatus == NOERROR
								? renderer::CapabilityReason::none
								: renderer::CapabilityReason::transactionFailed,
							childHookStatus);
					if (childHookStatus != NOERROR)
						DebugOut(L"Child process relay hook is unavailable");
				}
				else
				{
					PublishUnavailableCapability(
						renderer::HookCapability::childInjection, 0,
						CreateProcessInternalW != nullptr,
						renderer::CapabilityReason::explicitlyDisabled);
				}
#endif
				if (IsRunAsUser() && bEnableDW)
				{
					StartDirectWriteLifecycle();
				}
				else
				{
					PublishUnavailableCapability(
						renderer::HookCapability::directWrite, 0, false,
						renderer::CapabilityReason::explicitlyDisabled);
				}
				if (bUseFontSubstitute) {
					HookFontCreation();
				}
				else
				{
					PublishUnavailableCapability(
						renderer::HookCapability::fontSubstitution, 0, true,
						renderer::CapabilityReason::explicitlyDisabled);
				}
			}
			else
			{
				PublishUnavailableCapability(
					renderer::HookCapability::gdi, 0, true,
					renderer::CapabilityReason::explicitlyDisabled);
				PublishUnavailableCapability(
					renderer::HookCapability::directWrite, 0, false,
					renderer::CapabilityReason::explicitlyDisabled);
				PublishUnavailableCapability(
					renderer::HookCapability::fontSubstitution, 0, true,
					renderer::CapabilityReason::explicitlyDisabled);
			}
			if (IsUnload)
			{
				auto mutex_offical = renderer_raii::AdoptHandle(OpenMutex(MUTEX_ALL_ACCESS, false, _T("{46AD3688-30D0-411e-B2AA-CB177818F428}")));
				auto mutex_gditray2 = renderer_raii::AdoptHandle(OpenMutex(MUTEX_ALL_ACCESS, false, _T("Global\\MacType")));
				if (!mutex_gditray2)
					mutex_gditray2 = renderer_raii::AdoptHandle(OpenMutex(MUTEX_ALL_ACCESS, false, _T("MacType")));
				auto mutex_CompMode = renderer_raii::AdoptHandle(OpenMutex(MUTEX_ALL_ACCESS, false, _T("Global\\MacTypeCompMode")));
				if (!mutex_CompMode)
					mutex_CompMode = renderer_raii::AdoptHandle(OpenMutex(MUTEX_ALL_ACCESS, false, _T("MacTypeCompMode")));
				BOOL HookMode = (mutex_offical || (mutex_gditray2 && mutex_CompMode)) || (!mutex_offical && !mutex_gditray2);	//是否在兼容模式下
				if (!HookMode) {	//非兼容模式下，拒绝加载
					DebugOut(L"Process is in unloaddll list, exiting");
					return false;
				}
			}
			if (!processExcluded && !IsUnload &&
				!renderer::AcquireProcessRendererLease(instance))
			{
				DebugOut(L"Can't acquire the renderer unload lease, exiting");
				return FALSE;
			}
			if (!runtimeStart.Complete())
				return FALSE;
			if (processExcluded)
			{
				renderer::PublishRendererAdmission(
					renderer::RendererAdmission::quietSkip,
					renderer::RendererAdmissionReason::processExcluded);
			}
			else if (IsUnload)
			{
				renderer::PublishRendererAdmission(
					renderer::RendererAdmission::quietSkip,
					renderer::RendererAdmissionReason::processUnloadRequested);
			}
			else
			{
				renderer::PublishRendererAdmission(
					renderer::RendererAdmission::active);
			}
			break;
		}
		case DLL_THREAD_ATTACH:
#ifdef EASYHOOK
			EZHookMain(instance, reason, lpReserved);
#endif
			break;
		case DLL_THREAD_DETACH:
			g_TLInfo.ThreadTerm();
#ifdef EASYHOOK
			EZHookMain(instance, reason, lpReserved);
#endif
			break;
		case DLL_PROCESS_DETACH:
			if (!bDllInited)
				return true;
			// Process termination owns the address space and has already removed
			// peer threads. Cleanup here would run under the loader lock against
			// potentially torn-down dependencies.
			if (lpReserved != nullptr)
				return true;
			bDllInited = false;
			if (InterlockedExchange(&g_bHookEnabled, FALSE))
			{
				// A balanced active renderer reaches detach only after SafeUnload.
				// This is best-effort containment for an unsupported unmatched
				// FreeLibrary; heavyweight owners are never destroyed here.
				renderer::ProcessHookCoordinator().BeginStop();
				RestoreDirectWriteVtableHooks();
				hook_term();
				if (renderer::ProcessHookCoordinator().phase() ==
					renderer::RuntimePhase::stopping)
					renderer::ProcessHookCoordinator().CompleteStop();
			}

			// SafeUnload or verified QuietSkip already drained FreeType,
			// renderer policy, substitution, and settings outside loader lock.
			// Keep only final TLS-slot and empty lock-storage release here.
			g_TLInfo.ProcessTerm();
			CCriticalSectionLock::Term();
			COwnedCriticalSectionLock::Term();
#ifdef EASYHOOK
			EZHookMain(instance, reason, lpReserved);
#endif
			break;
		}
		return TRUE;
	}
	catch(...) {
		return FALSE;
	}
}
//EOF
