#include "unload_lifecycle.h"

#include <cstdint>

namespace renderer {

namespace {

std::atomic<HMODULE> g_rendererLease{nullptr};

} // namespace

RendererProviderDrainTransaction::RendererProviderDrainTransaction(
	const UnloadProviderAdapter* providers,
	std::size_t count) noexcept
{
	if (providers == nullptr || count == 0 || count > kMaximumProviders)
		return;
	for (std::size_t index = 0; index < count; ++index)
	{
		if (providers[index].prepare == nullptr ||
			providers[index].abort == nullptr ||
			providers[index].commit == nullptr)
			return;
		providers_[index] = providers[index];
	}
	count_ = count;
	valid_ = true;
}

RendererProviderDrainTransaction::~RendererProviderDrainTransaction()
{
	Abort();
}

RendererProviderDrainTransaction::RendererProviderDrainTransaction(
	RendererProviderDrainTransaction&& other) noexcept
	: providers_(other.providers_), prepared_(other.prepared_),
	  count_(other.count_), valid_(other.valid_), settled_(other.settled_)
{
	other.count_ = 0;
	other.valid_ = false;
	other.settled_ = true;
	other.prepared_.fill(false);
}

RendererProviderDrainTransaction&
RendererProviderDrainTransaction::operator=(
	RendererProviderDrainTransaction&& other) noexcept
{
	if (this == &other)
		return *this;
	Abort();
	providers_ = other.providers_;
	prepared_ = other.prepared_;
	count_ = other.count_;
	valid_ = other.valid_;
	settled_ = other.settled_;
	other.count_ = 0;
	other.valid_ = false;
	other.settled_ = true;
	other.prepared_.fill(false);
	return *this;
}

bool RendererProviderDrainTransaction::Prepare(
	DWORD timeoutMilliseconds) noexcept
{
	if (!valid_ || settled_)
		return false;
	for (std::size_t index = 0; index < count_; ++index)
	{
		UnloadProviderPreparation const preparation =
			providers_[index].prepare(
				providers_[index].context, timeoutMilliseconds);
		if (preparation == UnloadProviderPreparation::prepared)
		{
			prepared_[index] = true;
		}
		else if (preparation == UnloadProviderPreparation::unsafeToUnload)
		{
			Abort();
			return false;
		}
	}
	return true;
}

void RendererProviderDrainTransaction::Abort() noexcept
{
	if (settled_)
		return;
	for (std::size_t index = count_; index != 0; --index)
	{
		std::size_t const provider = index - 1;
		if (!prepared_[provider])
			continue;
		providers_[provider].abort(providers_[provider].context);
		prepared_[provider] = false;
	}
	settled_ = true;
}

bool RendererProviderDrainTransaction::Commit(
	DWORD timeoutMilliseconds) noexcept
{
	if (!valid_ || settled_)
		return false;
	for (std::size_t index = 0; index < count_; ++index)
	{
		if (!prepared_[index])
			continue;
		if (!providers_[index].commit(
			providers_[index].context, timeoutMilliseconds))
		{
			// A provider commit may have detached part of its state. Leave that
			// provider in its retryable stop phase, but roll back providers that
			// have not begun commit yet.
			prepared_[index] = false;
			for (std::size_t later = count_; later > index + 1; --later)
			{
				std::size_t const provider = later - 1;
				if (prepared_[provider])
				{
					providers_[provider].abort(providers_[provider].context);
					prepared_[provider] = false;
				}
			}
			settled_ = true;
			return false;
		}
		prepared_[index] = false;
	}
	settled_ = true;
	return true;
}

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
