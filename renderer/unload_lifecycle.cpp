#include "unload_lifecycle.h"

#include <cstdint>

namespace renderer {

namespace {

std::atomic<HMODULE> g_rendererLease{nullptr};

} // namespace

bool UnloadAttemptGate::TryBegin() noexcept
{
	bool expected = false;
	return active_.compare_exchange_strong(
		expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

void UnloadAttemptGate::EndForRetry() noexcept
{
	active_.store(false, std::memory_order_release);
}

bool UnloadAttemptGate::active() const noexcept
{
	return active_.load(std::memory_order_acquire);
}

UnloadAttemptGate& ProcessUnloadAttemptGate() noexcept
{
	static UnloadAttemptGate gate;
	return gate;
}

bool AcquireProcessRendererLease(HMODULE module) noexcept
{
	if (module == nullptr)
		return false;
	if (g_rendererLease.load(std::memory_order_acquire) != nullptr)
		return true;

	HMODULE lease = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCWSTR>(module),
			&lease))
		return false;

	HMODULE expected = nullptr;
	if (!g_rendererLease.compare_exchange_strong(
			expected, lease, std::memory_order_release, std::memory_order_acquire)) {
		FreeLibrary(lease);
	}
	return true;
}

HMODULE TakeProcessRendererLease() noexcept
{
	return g_rendererLease.exchange(nullptr, std::memory_order_acq_rel);
}

bool ProcessRendererLeaseHeld() noexcept
{
	return g_rendererLease.load(std::memory_order_acquire) != nullptr;
}

} // namespace renderer
