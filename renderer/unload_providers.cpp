#include "unload_lifecycle.h"

#include "directwrite.h"
#include "unity_font_hook.h"

namespace renderer {
namespace {

UnloadProviderPreparation PrepareDirectWrite(void*, DWORD timeout) noexcept
{
	switch (PrepareDirectWriteLifecycleStop(timeout))
	{
	case DirectWriteLifecycleStopPreparation::prepared:
		return UnloadProviderPreparation::prepared;
	case DirectWriteLifecycleStopPreparation::alreadyStopped:
		return UnloadProviderPreparation::alreadyStopped;
	case DirectWriteLifecycleStopPreparation::unsafeToUnload:
	default:
		return UnloadProviderPreparation::unsafeToUnload;
	}
}

void AbortDirectWrite(void*) noexcept
{
	AbortDirectWriteLifecycleStop();
}

bool CommitDirectWrite(void*, DWORD timeout) noexcept
{
	return CommitDirectWriteLifecycleStop(timeout);
}

UnloadProviderPreparation PrepareUnity(void*, DWORD timeout) noexcept
{
	switch (PrepareUnityFontHookLifecycleStop(timeout))
	{
	case UnityFontHookStopPreparation::prepared:
		return UnloadProviderPreparation::prepared;
	case UnityFontHookStopPreparation::alreadyStopped:
		return UnloadProviderPreparation::alreadyStopped;
	case UnityFontHookStopPreparation::unsafeToUnload:
	default:
		return UnloadProviderPreparation::unsafeToUnload;
	}
}

void AbortUnity(void*) noexcept
{
	AbortUnityFontHookLifecycleStop();
}

bool CommitUnity(void*, DWORD) noexcept
{
	return CommitUnityFontHookLifecycleStop();
}

} // namespace

RendererProviderDrainTransaction
MakeProcessRendererProviderDrainTransaction() noexcept
{
	const UnloadProviderAdapter providers[] = {
		{nullptr, PrepareDirectWrite, AbortDirectWrite, CommitDirectWrite},
		{nullptr, PrepareUnity, AbortUnity, CommitUnity},
	};
	return RendererProviderDrainTransaction(providers, _countof(providers));
}

} // namespace renderer
