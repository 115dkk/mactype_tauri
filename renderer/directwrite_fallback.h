#pragma once

#include "common.h"

bool HookDirectWriteSystemFallback(
	IDWriteFactory* factory,
	IDWriteFontCollection* systemCollection) noexcept;
void RestoreDirectWriteFallbackVtableHooks() noexcept;
void ClearDirectWriteFallbackSources() noexcept;
void SetFallbackDiagnosticSink(
	void (*sink)(wchar_t const*) noexcept) noexcept;
