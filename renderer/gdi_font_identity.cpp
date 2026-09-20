#include "gdi_font_identity.h"

#include "arrival_evidence.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>

namespace renderer {
namespace gdi_font_identity {
namespace {

constexpr std::array<const wchar_t*, 6> kGlyphIndexTextStackModules{{
	L"Qt5Gui.dll",
	L"Qt6Gui.dll",
	L"QtGui4.dll",
	L"Qt5Guid.dll",
	L"Qt6Guid.dll",
	L"QtGuid4.dll",
}};

std::atomic<int> stockIdentityVerdict{0};

bool IsModuleLoaded(const wchar_t* name) noexcept
{
	return GetModuleHandleW(name) != nullptr;
}

} // namespace

bool IsGlyphIndexTextStackModule(const wchar_t* baseName) noexcept
{
	if (baseName == nullptr)
		return false;

	for (const wchar_t* candidate : kGlyphIndexTextStackModules)
	{
		if (CompareStringOrdinal(baseName, -1, candidate, -1, TRUE) == CSTR_EQUAL)
			return true;
	}
	return false;
}

bool GlyphIndexTextStackLoaded(bool (*isLoaded)(const wchar_t*)) noexcept
{
	if (isLoaded == nullptr)
		return false;

	for (const wchar_t* candidate : kGlyphIndexTextStackModules)
	{
		if (isLoaded(candidate))
			return true;
	}
	return false;
}

bool CommitsStockIdentity(
	unsigned long gdiObjectsAtHookInstall,
	bool glyphIndexTextStackLoaded) noexcept
{
	return gdiObjectsAtHookInstall != 0 && glyphIndexTextStackLoaded;
}

bool StockIdentityCommitted() noexcept
{
	int const verdict = stockIdentityVerdict.load();
	if (verdict != 0)
		return verdict > 0;

	// Freeze the verdict because changing font identity after an engine caches
	// an HFONT cmap would recreate the mismatch this policy prevents.
	arrival_evidence::ArrivalEvidence const* const evidence =
		arrival_evidence::Recorded();
	bool const committed = CommitsStockIdentity(
		evidence == nullptr ? 0U : evidence->gdiObjectsAtInstall,
		GlyphIndexTextStackLoaded(IsModuleLoaded));
	stockIdentityVerdict.store(committed ? 1 : -1);
	return committed;
}

void ResetForTesting() noexcept
{
	stockIdentityVerdict.store(0);
}

} // namespace gdi_font_identity
} // namespace renderer
