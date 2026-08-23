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

// Active renderers keep one owned reference to their own image. A balanced
// caller release of its own LoadLibrary reference cannot unmap live hooks;
// Windows references are not owner-tagged, so unmatched releases are outside
// the supported contract. SafeUnload takes this lease only after quiescence.
bool AcquireProcessRendererLease(HMODULE module) noexcept;
HMODULE TakeProcessRendererLease() noexcept;
bool ProcessRendererLeaseHeld() noexcept;

// Releases renderer-owned heavyweight resources in dependency order. The
// supported callers are SafeUnload and the verified quiet-skip evidence path,
// both of which execute outside the loader lock after admission has closed.
bool DrainProcessRendererResourcesOutsideLoaderLock() noexcept;

} // namespace renderer
