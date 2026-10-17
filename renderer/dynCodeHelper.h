#pragma once

#include "common.h"
#include <mutex>

class AutoEnableDynamicCodeGen
{
public:
	explicit AutoEnableDynamicCodeGen(bool enable = true);
	~AutoEnableDynamicCodeGen();

private:
	bool enabled;

	using PSET_THREAD_INFORMATION_PROC = BOOL
		(WINAPI *)(
		_In_ HANDLE                   hThread,
		_In_ THREAD_INFORMATION_CLASS ThreadInformationClass,
		_In_reads_bytes_(ThreadInformationSize) PVOID ThreadInformation,
		_In_ DWORD                    ThreadInformationSize
		);

	using PGET_THREAD_INFORMATION_PROC = BOOL
		(WINAPI *)(
		_In_ HANDLE                   hThread,
		_In_ THREAD_INFORMATION_CLASS ThreadInformationClass,
		_Out_writes_bytes_(ThreadInformationSize) PVOID ThreadInformation,
		_In_ DWORD                    ThreadInformationSize
		);

	static PSET_THREAD_INFORMATION_PROC SetThreadInformationProc;
	static PGET_THREAD_INFORMATION_PROC GetThreadInformationProc;
	static PROCESS_MITIGATION_DYNAMIC_CODE_POLICY processPolicy;
	static std::once_flag processPolicyOnce;
};
