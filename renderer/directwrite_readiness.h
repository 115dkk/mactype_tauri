#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace directwrite_readiness {

// How long a key whose builds keep failing waits before it may build again;
// empty once it has failed often enough that it stays on the stock set.
[[nodiscard]] inline std::optional<std::chrono::milliseconds> FailureRetryDelay(
	unsigned failedBuilds) noexcept
{
	switch (failedBuilds)
	{
	case 0:
	case 1:
		return std::chrono::seconds(2);
	case 2:
		return std::chrono::seconds(15);
	case 3:
		return std::chrono::seconds(120);
	default:
		return std::nullopt;
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

// The fallback lookup reads the entry built from the factory's canonical
// source. A build reserved late for a superseded source is inserted after the
// current one, so insertion order alone would let it stand in for the current
// source; the newest entry serves only until the canonical source has one.
template <typename Container, typename Predicate, typename CanonicalPredicate>
typename Container::value_type CanonicalOrNewestMatching(
	Container const& entries,
	Predicate matches,
	CanonicalPredicate isCanonical)
{
	typename Container::value_type const canonical = NewestMatching(
		entries,
		[&](typename Container::value_type const& candidate) {
			return matches(candidate) && isCanonical(candidate);
		});
	if (canonical)
		return canonical;
	return NewestMatching(entries, matches);
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

// Counts modern collection calls on this thread, so the legacy hook can tell
// that DirectWrite answered it through the patched modern slot.
inline std::uint64_t& ModernCollectionCalls() noexcept
{
	static thread_local std::uint64_t calls = 0;
	return calls;
}

} // namespace directwrite_readiness
