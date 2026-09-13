#include "activation_repaint.h"

namespace renderer {
namespace {

struct RepaintContext
{
	DWORD processId;
	unsigned int invalidated;
};

BOOL CALLBACK RepaintWindow(HWND window, LPARAM parameter) noexcept
{
	RepaintContext* context = reinterpret_cast<RepaintContext*>(parameter);
	DWORD processId = 0;
	GetWindowThreadProcessId(window, &processId);
	if (processId != context->processId || !IsWindowVisible(window))
		return TRUE;

	if (RedrawWindow(
			window,
			nullptr,
			nullptr,
			RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN))
	{
		++context->invalidated;
	}
	return TRUE;
}

} // namespace

unsigned int RepaintOwnedWindows(DWORD processId) noexcept
{
	RepaintContext context{processId, 0};
	EnumWindows(RepaintWindow, reinterpret_cast<LPARAM>(&context));
	return context.invalidated;
}

} // namespace renderer
