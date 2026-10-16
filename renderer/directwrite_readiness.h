#pragma once

#include <chrono>

namespace directwrite_readiness {

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
