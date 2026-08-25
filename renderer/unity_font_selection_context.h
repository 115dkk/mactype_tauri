#pragma once

namespace renderer { namespace unity {

enum class FontRefSelection : unsigned char
{
	nativeFamily,
	mappedFamily,
};

// Keeps Unity's selected FontRef family paired with any nested private
// FreeType face-open call. Builds without an exact FontRef resolver retain the
// legacy path-based fallback; an observed native family fails closed.
class ScopedFontRefSelectionContext final
{
public:
	explicit ScopedFontRefSelectionContext(FontRefSelection selection) noexcept;
	~ScopedFontRefSelectionContext();

	ScopedFontRefSelectionContext(
		const ScopedFontRefSelectionContext&) = delete;
	ScopedFontRefSelectionContext& operator=(
		const ScopedFontRefSelectionContext&) = delete;

private:
	unsigned char previous_;
};

bool CurrentFontRefAllowsFaceRedirect() noexcept;

}} // namespace renderer::unity
