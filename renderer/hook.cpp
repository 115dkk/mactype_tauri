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
// typedef int (WINAPI *PFNGdipCreateFontFamilyFromName)(const WCHAR *name, void *fontCollection, void **FontFamily);
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
HANDLE						g_hfDbgText;
#endif

//void InstallManagerHook();
//void RemoveManagerHook();

//#include "APITracer.hpp"

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

	//
	//  We now know the groups associated with this token.  We want to look to	see if
		//  the interactive group is active in the token, and if so, we know that
		//  this is an interactive process.
		//
		//  We also look for the "service" SID, and if it's present, we know we're a service.
		//
		//  The service SID will be present iff the service is running in a
		//  user account (and was invoked by the service controller).
		//


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

		//
		//  Check to see if the group we're looking at is one of
		//  the 2 groups we're interested in.
		//

	if (EqualSid(Sid, interactiveSid.get()))
		{
			//
			//  This process has the Interactive SID in its
			//  token.  This means that the process is running as
			//  an EXE.
			//
			fExe = true;
			goto ret;
		}
		else if (EqualSid(Sid, serviceSid.get()))
		{
			//
			//  This process has the Service SID in its
			//  token.  This means that the process is running as
			//  a service running in a user account.
			//
			fExe = FALSE;
			goto ret;
		}
	}

	//
	//  Neither Interactive or Service was present in the current users token,
	//  This implies that the process is running as a service, most likely
	//  running as LocalSystem.
	//
	fExe = FALSE;

ret:
// 	EventLogging logger;
// 	TCHAR s[100] = { 0 };
// 	wsprintf(s, L"Loading processid %d, isUserProcess=%d", GetCurrentProcessId(), (int)fExe);
// 	LPCTSTR lpStrings[] = {s};
// 	logger.LogIt(1, 1, lpStrings, 1);
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
#ifdef DEBUG
			//MessageBox(0, L"Load", nullptr, MB_OK);
#endif
			DebugOut(L"Begin core loading stage, pid %d", ::GetCurrentProcessId());
			if (bDllInited)
				return true;
			if (!ValidateRendererProfile(instance))
				return FALSE;
			RuntimeStartGuard runtimeStart;
			if (!runtimeStart.owns_start())
				return FALSE;
			g_dllInstance = instance;
#ifdef EASYHOOK
			EZHookMain(instance, reason, lpReserved);
#endif
			//初期化順序
			//DLL_PROCESS_DETACHではこれの逆順にする
			//1. CRT関数の初期化
			//2. クリティカルセクションの初期化
			//3. TLSの準備
			//4. CGdippSettingsのインスタンス生成、INI読み込み
			//5. ExcludeModuleチェック
			// 6. FreeTypeライブラリの初期化
			// 7. FreeTypeFontEngineのインスタンス生成
			// 8. APIをフック
			// 9. ManagerのGetProcAddressをフック

			//1
			_CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_LEAK_CHECK_DF);
			_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_WNDW);
			//_CrtSetBreakAlloc(100);

			//Operaよ止まれ～
			//Assert(GetModuleHandleA("opera.exe") == nullptr);

			//setlocale(LC_ALL, "");
			g_hinstDLL = instance;


			//APITracer::Start(instance, APITracer::OutputFile);

					//2, 3
			CCriticalSectionLock::Init();
			COwnedCriticalSectionLock::Init();
			CThreadCounter::Init();
			if (!g_TLInfo.ProcessInit()) {
				DebugOut(L"Can't initialize process, exiting");
				return FALSE;
			}

			// Above classes are heavily referenced and must be initialized as early as possible.
			// Unload dll is not safe until their initialization is complete.
			bDllInited = true;

			//4
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
			//5
			if (!IsProcessExcluded() && !IsUnload) {
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

				//if (!AddEasyHookEnv()) return FALSE;	//fail to load easyhook
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
				//hook d2d if already loaded
	/*
				DWORD dwSessionID = 0;
				if (ProcessIdToSessionIdProc)
					ProcessIdToSessionIdProc(GetCurrentThreadId(), &dwSessionID);
				else
					dwSessionID = 1;*/
				if (IsRunAsUser() && bEnableDW)
				{
					StartDirectWriteLifecycle();
					//hook_demand_LdrLoadDll();
				}
				else
				{
					PublishUnavailableCapability(
						renderer::HookCapability::directWrite, 0, false,
						renderer::CapabilityReason::explicitlyDisabled);
				}
				// only hook font creation funcs if font substition is set.
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
			//获得当前加载模式

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

			//APITracer::Finish();
			if (!runtimeStart.Complete())
				return FALSE;
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
			//		RemoveManagerHook();
			if (!bDllInited)
				return true;
			renderer::ProcessHookCoordinator().BeginStop();
			bDllInited = false;
			if (InterlockedExchange(&g_bHookEnabled, FALSE) && lpReserved == nullptr)
			{ // 如果是进程终止，则不需要释放
				RestoreDirectWriteVtableHooks();
				hook_term();
				// delete AACacheFull;
				// delete AACache;
				// 			for (int i=0;i<CACHE_SIZE;i++)
				// 				delete g_AACache2[i];	//清除缓磥E
				// free(g_charmapCache);
			}
#ifndef DEBUG
			if (lpReserved != nullptr) return true;
#endif

			DestroyFreeTypeFontEngine();

#ifdef INFINALITY
			// enable infinality exclusive features
			FT_freeEnv();
#endif
			//if (g_alterGUIFont)
			//	DeleteObject(g_alterGUIFont);
			FontLFree();
			/*
			#ifndef _WIN64
					__FUnloadDelayLoadedDLL2("easyhook32.dll");
			#else
					__FUnloadDelayLoadedDLL2("easyhook64.dll");
			#endif*/

			CGdippSettings::DestroyInstance();
			g_TLInfo.ProcessTerm();
			CCriticalSectionLock::Term();
			COwnedCriticalSectionLock::Term();
#ifdef EASYHOOK
			EZHookMain(instance, reason, lpReserved);
#endif
			if (renderer::ProcessHookCoordinator().phase() ==
				renderer::RuntimePhase::stopping)
				renderer::ProcessHookCoordinator().CompleteStop();
			break;
		}
		return TRUE;
	}
	catch(...) {
		return FALSE;
	}
}
//EOF
