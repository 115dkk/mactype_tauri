#pragma once

#include <windows.h>

#include <atomic>
#include <array>
#include <cstddef>

namespace renderer {

enum class UnloadProviderPreparation : unsigned char
{
	prepared,
	alreadyStopped,
	unsafeToUnload,
};

struct UnloadProviderAdapter final
{
	void* context = nullptr;
	UnloadProviderPreparation (*prepare)(void*, DWORD) noexcept = nullptr;
	void (*abort)(void*) noexcept = nullptr;
	bool (*commit)(void*, DWORD) noexcept = nullptr;
};

class RendererProviderDrainTransaction final
{
public:
	RendererProviderDrainTransaction(
		const UnloadProviderAdapter* providers,
		std::size_t count) noexcept;
	~RendererProviderDrainTransaction();
	RendererProviderDrainTransaction(
		RendererProviderDrainTransaction&& other) noexcept;
	RendererProviderDrainTransaction& operator=(
		RendererProviderDrainTransaction&& other) noexcept;
	RendererProviderDrainTransaction(
		const RendererProviderDrainTransaction&) = delete;
	RendererProviderDrainTransaction& operator=(
		const RendererProviderDrainTransaction&) = delete;

	bool Prepare(DWORD timeoutMilliseconds = 3000) noexcept;
	void Abort() noexcept;
	bool Commit(DWORD timeoutMilliseconds = 3000) noexcept;

private:
	static constexpr std::size_t kMaximumProviders = 8;
	std::array<UnloadProviderAdapter, kMaximumProviders> providers_{};
	std::array<bool, kMaximumProviders> prepared_{};
	std::size_t count_ = 0;
	bool valid_ = false;
	bool settled_ = false;
};

RendererProviderDrainTransaction MakeProcessRendererProviderDrainTransaction() noexcept;

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
