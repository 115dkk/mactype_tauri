#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

enum class UnityFontHookStopPreparation : unsigned char
{
	prepared,
	alreadyStopped,
	unsafeToUnload,
};

void StartUnityFontHookLifecycle();
UnityFontHookStopPreparation PrepareUnityFontHookLifecycleStop(
	DWORD timeoutMilliseconds = 3000);
void AbortUnityFontHookLifecycleStop();
bool CommitUnityFontHookLifecycleStop();
