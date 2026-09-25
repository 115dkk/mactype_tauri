#pragma once

#include "common.h"

bool HookDirectWriteSystemFallback(
	IDWriteFactory* factory,
	IDWriteFontCollection* systemCollection) noexcept;
void RestoreDirectWriteFallbackVtableHooks() noexcept;
void ClearDirectWriteFallbackSources() noexcept;
