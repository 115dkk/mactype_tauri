#pragma once

namespace renderer {
namespace gdi_font_identity {

// Modules whose text engines read one HFONT's cmap and draw those glyph ids
// through another HFONT of the same LOGFONT (Qt 4/5/6 GUI, release and debug).
// The name is a null-terminated module base name; null is not a module.
bool IsGlyphIndexTextStackModule(const wchar_t* baseName) noexcept;

// Runs the probe over the module list; production passes a GetModuleHandleW probe.
bool GlyphIndexTextStackLoaded(bool (*isLoaded)(const wchar_t*)) noexcept;

// The decision itself, kept free of process state so it can be tested:
// true when GDI objects already existed at hook installation and such a
// text stack is loaded.
bool CommitsStockIdentity(
	unsigned long gdiObjectsAtHookInstall,
	bool glyphIndexTextStackLoaded) noexcept;

// Memoised process verdict: the recorded arrival evidence combined with the
// module probe on the first call. Never throws.
bool StockIdentityCommitted() noexcept;

// Test seam for the memoised policy verdict.
void ResetForTesting() noexcept;

} // namespace gdi_font_identity
} // namespace renderer
