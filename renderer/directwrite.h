#include "common.h"
#include <VersionHelpers.h>

#pragma once

#ifdef EASYHOOK

#include "easyhook.h"
#define HOOK_MANUALLY HOOK_DEFINE
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	extern rettype(WINAPI * ORIG_##name) argtype;  \
	extern HOOK_TRACE_INFO HOOK_##name;
#include "hooklist.h"
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#define HOOK_MANUALLY(rettype, name, argtype, arglist) \
	extern LONG hook_demand_##name(bool bForce = false);
#define HOOK_DEFINE(rettype, name, argtype, arglist) ;
#include "hooklist.h"
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#else

#define HOOK_MANUALLY HOOK_DEFINE
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	extern rettype(WINAPI * ORIG_##name) argtype;  \
	extern BOOL IsHooked_##name; \
	extern rettype WINAPI REF_##name argtype;
#include "hooklist.h"
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#define HOOK_MANUALLY(rettype, name, argtype, arglist) \
	extern LONG hook_demand_##name(bool bForce = false);
#define HOOK_DEFINE(rettype, name, argtype, arglist) ;
#include "hooklist.h"
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#endif

enum class DirectWriteLifecycleStopPreparation : unsigned char
{
	prepared,
	alreadyStopped,
	unsafeToUnload,
};

void StartDirectWriteLifecycle();
// Blocks until the DirectWrite lifecycle has left its starting phase and any
// pre-existing-factory hook worker has hooked the known factories, or until
// timeoutMs passes. Returns immediately when the lifecycle never started or
// is already stopping.
void WaitForDirectWriteHooksSettled(DWORD timeoutMs);
DirectWriteLifecycleStopPreparation PrepareDirectWriteLifecycleStop(
	DWORD timeoutMilliseconds = 3000);
void AbortDirectWriteLifecycleStop();
bool CommitDirectWriteLifecycleStop(DWORD timeoutMilliseconds = 3000);
bool RestoreDirectWriteVtableHooks(DWORD timeoutMilliseconds = 3000);
