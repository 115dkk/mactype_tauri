#pragma once

#include <chrono>

namespace directwrite_readiness {

// Stores how long a key whose builds keep failing waits before it may build
// again. False once it has failed often enough that it stays on the stock set.
// The renderer project builds below C++17, so there is no std::optional here.
inline bool FailureRetryDelay(
	unsigned failedBuilds,
	std::chrono::milliseconds& delay) noexcept
{
	switch (failedBuilds)
	{
	case 0:
	case 1:
		delay = std::chrono::seconds(2);
		return true;
	case 2:
		delay = std::chrono::seconds(15);
		return true;
	case 3:
		delay = std::chrono::seconds(120);
		return true;
	default:
		delay = std::chrono::milliseconds::zero();
		return false;
	}
}

enum class CompletedBuildDisposition
{
	useCached,
	retry,
};

// The caller holds the cache mutex and only passes a completed entry.
template <typename Entry, typename TimePoint, typename IsApplied>
CompletedBuildDisposition PrepareCompletedBuild(
	Entry& entry,
	TimePoint now,
	IsApplied isApplied)
{
	if (isApplied(entry.status) || !entry.retryAllowed || now < entry.retryAfter)
		return CompletedBuildDisposition::useCached;
	entry.building = true;
	entry.reservingThread = 0;
	return CompletedBuildDisposition::retry;
}

// Several entries can share a factory and generation after its source set
// changes; the newest entry belongs to the current source.
template <typename Container, typename Predicate>
typename Container::value_type NewestMatching(
	Container const& entries,
	Predicate matches)
{
	for (auto candidate = entries.rbegin();
		candidate != entries.rend(); ++candidate)
	{
		if (matches(*candidate))
			return *candidate;
	}
	return typename Container::value_type{};
}

// The caller publishes the candidate before building so every entry point uses
// one canonical source. A publisher that loses the race reloads the winner.
template <typename Factory, typename Source, typename GetPublished,
	typename AcquireOriginal, typename Publish>
Source EstablishSource(
	Factory* factory,
	Source candidate,
	GetPublished getPublished,
	AcquireOriginal acquireOriginal,
	Publish publish)
{
	Source published = getPublished(factory);
	if (published)
		return published;
	if (!candidate && !acquireOriginal(factory, candidate))
		return Source();
	if (!candidate)
		return Source();
	publish(factory, candidate);
	return getPublished(factory);
}

// Both collection calls and fallback mapping use this wait. The building
// thread declines the wait so a nested DirectWrite call cannot wait on itself.
template <typename Entry, typename Lock, typename Rep, typename Period>
bool WaitForBuild(
	Entry const& entry,
	Lock& lock,
	std::chrono::duration<Rep, Period> const& timeout,
	bool mayWait)
{
	if (!entry->building)
		return true;
	return mayWait && entry->ready.wait_for(
		lock, timeout, [&entry] { return !entry->building; });
}

// A thread that is establishing a factory's canonical source must not run
// the preamble again when DirectWrite re-enters the patched virtual slots
// from inside the original call; the nested scope reports itself so the
// caller hands that inner call the stock object.
class CanonicalSourceScope
{
public:
	CanonicalSourceScope() noexcept : nested_(Establishing())
	{
		Establishing() = true;
	}
	~CanonicalSourceScope()
	{
		if (!nested_)
			Establishing() = false;
	}
	CanonicalSourceScope(CanonicalSourceScope const&) = delete;
	CanonicalSourceScope& operator=(CanonicalSourceScope const&) = delete;
	bool nested() const noexcept { return nested_; }

private:
	static bool& Establishing() noexcept
	{
		static thread_local bool establishing = false;
		return establishing;
	}

	bool nested_ = false;
};

} // namespace directwrite_readiness
