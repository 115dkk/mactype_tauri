#pragma once

#include <windows.h>

#include <atomic>

namespace renderer {

// Serializes the prepare/commit unload protocol while allowing a failed
// attempt to publish retry admission again.
class UnloadAttemptGate
{
public:
	bool TryBegin() noexcept;
	void EndForRetry() noexcept;
	bool active() const noexcept;

private:
	std::atomic<bool> active_{false};
};

UnloadAttemptGate& ProcessUnloadAttemptGate() noexcept;

// Active renderers keep one owned reference to their own image. An arbitrary
// FreeLibrary can release the caller's reference but cannot unmap live hooks.
// SafeUnload takes this lease only after the renderer has reached quiescence.
bool AcquireProcessRendererLease(HMODULE module) noexcept;
HMODULE TakeProcessRendererLease() noexcept;
bool ProcessRendererLeaseHeld() noexcept;

} // namespace renderer
