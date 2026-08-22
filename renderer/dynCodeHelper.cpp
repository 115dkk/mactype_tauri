#include "dynCodeHelper.h"

/*
* class AutoEnableDynamicCodeGen
*/

typedef
BOOL
(WINAPI *PGET_PROCESS_MITIGATION_POLICY_PROC)(
_In_  HANDLE                    hProcess,
_In_  PROCESS_MITIGATION_POLICY MitigationPolicy,
_Out_ PVOID                     lpBuffer,
_In_  SIZE_T                    dwLength
);

AutoEnableDynamicCodeGen::PSET_THREAD_INFORMATION_PROC AutoEnableDynamicCodeGen::SetThreadInformationProc = nullptr;
AutoEnableDynamicCodeGen::PGET_THREAD_INFORMATION_PROC AutoEnableDynamicCodeGen::GetThreadInformationProc = nullptr;
PROCESS_MITIGATION_DYNAMIC_CODE_POLICY AutoEnableDynamicCodeGen::processPolicy;
std::once_flag AutoEnableDynamicCodeGen::processPolicyOnce;

AutoEnableDynamicCodeGen::AutoEnableDynamicCodeGen(bool enable) : enabled(false)
{
	if (enable == false)
	{
		return;
	}

	// Dynamic-code policy is process-wide and established before hook admission;
	// cache it instead of resolving and querying the API for every hook.

	std::call_once(processPolicyOnce, []
		{
			PGET_PROCESS_MITIGATION_POLICY_PROC GetProcessMitigationPolicyProc = nullptr;

			HMODULE module = GetModuleHandleW(_T("api-ms-win-core-processthreads-l1-1-3.dll"));

			if (module != nullptr)
		{
			GetProcessMitigationPolicyProc = reinterpret_cast<PGET_PROCESS_MITIGATION_POLICY_PROC>(GetProcAddress(module, "GetProcessMitigationPolicy"));
			SetThreadInformationProc = reinterpret_cast<PSET_THREAD_INFORMATION_PROC>(GetProcAddress(module, "SetThreadInformation"));
			GetThreadInformationProc = reinterpret_cast<PGET_THREAD_INFORMATION_PROC>(GetProcAddress(module, "GetThreadInformation"));
		}

		if ((GetProcessMitigationPolicyProc == nullptr) ||
			(!GetProcessMitigationPolicyProc(GetCurrentProcess(), ProcessDynamicCodePolicy, &processPolicy, sizeof(processPolicy))))
		{
			processPolicy.ProhibitDynamicCode = 0;
		}
	});

	//
	// The process is not prohibiting dynamic code or does not allow threads
	// to opt out.  In either case, return to the caller.
	//
	// N.B. It is OK that this policy is mutable at runtime. If a process
	//      really does not allow thread opt-out, then the call below will fail
	//      benignly.
	//

	if ((processPolicy.ProhibitDynamicCode == 0) || (processPolicy.AllowThreadOptOut == 0))
	{
		return;
	}

	if (SetThreadInformationProc == nullptr || GetThreadInformationProc == nullptr)
	{
		return;
	}

	// 
	// If dynamic code is already allowed for this thread, then don't attempt to allow it again.
	//

	DWORD threadPolicy;

	if ((GetThreadInformationProc(GetCurrentThread(), ThreadDynamicCodePolicy, &threadPolicy, sizeof(DWORD))) &&
		(threadPolicy == THREAD_DYNAMIC_CODE_ALLOW))
	{
		return;
	}

	threadPolicy = THREAD_DYNAMIC_CODE_ALLOW;

	BOOL result = SetThreadInformationProc(GetCurrentThread(), ThreadDynamicCodePolicy, &threadPolicy, sizeof(DWORD));
	Assert(result);

	enabled = result != FALSE;
}

AutoEnableDynamicCodeGen::~AutoEnableDynamicCodeGen()
{
	if (enabled)
	{
		DWORD threadPolicy = 0;

		BOOL result = SetThreadInformationProc(GetCurrentThread(), ThreadDynamicCodePolicy, &threadPolicy, sizeof(DWORD));
		Assert(result);

		enabled = false;
	}
}
