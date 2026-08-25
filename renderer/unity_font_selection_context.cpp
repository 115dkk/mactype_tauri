#include "unity_font_selection_context.h"

namespace renderer { namespace unity {
namespace {

enum class SelectionState : unsigned char
{
	unobserved,
	nativeFamily,
	mappedFamily,
};

thread_local SelectionState g_selection = SelectionState::unobserved;

} // namespace

ScopedFontRefSelectionContext::ScopedFontRefSelectionContext(
	FontRefSelection selection) noexcept
	: previous_(static_cast<unsigned char>(g_selection))
{
	g_selection = selection == FontRefSelection::mappedFamily
		? SelectionState::mappedFamily
		: SelectionState::nativeFamily;
}

ScopedFontRefSelectionContext::~ScopedFontRefSelectionContext()
{
	g_selection = static_cast<SelectionState>(previous_);
}

bool CurrentFontRefAllowsFaceRedirect() noexcept
{
	return g_selection != SelectionState::nativeFamily;
}

}} // namespace renderer::unity
