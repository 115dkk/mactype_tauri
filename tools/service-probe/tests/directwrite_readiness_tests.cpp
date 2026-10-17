#include "../../../renderer/directwrite_readiness.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, char const* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct FakeEntry
{
    std::condition_variable ready;
    int aliases = 0;
    bool building = true;
};

struct FakeFactory
{
    std::mutex sourceMutex;
    std::shared_ptr<int> source;
    std::mutex cacheMutex;
    std::condition_variable stateChanged;
    std::shared_ptr<FakeEntry> entry;
    unsigned int waiters = 0;
    bool buildEntered = false;
    bool releaseBuild = false;
};

std::shared_ptr<int> PublishedSource(FakeFactory* factory)
{
    std::lock_guard<std::mutex> lock(factory->sourceMutex);
    return factory->source;
}

void PublishSource(FakeFactory* factory, std::shared_ptr<int> const& source)
{
    std::lock_guard<std::mutex> lock(factory->sourceMutex);
    if (!factory->source)
        factory->source = source;
}

int BuildAliases(FakeFactory* factory, std::shared_ptr<int> const& source)
{
    std::shared_ptr<FakeEntry> entry;
    bool builder = false;
    std::unique_lock<std::mutex> lock(factory->cacheMutex);
    if (!factory->entry)
    {
        factory->entry = std::make_shared<FakeEntry>();
        builder = true;
    }
    entry = factory->entry;
    if (!builder)
    {
        ++factory->waiters;
        factory->stateChanged.notify_all();
        bool const ready = directwrite_readiness::WaitForBuild(
            entry, lock, 10s, true);
        return ready ? entry->aliases : -1;
    }

    factory->buildEntered = true;
    factory->stateChanged.notify_all();
    factory->stateChanged.wait(lock, [factory] { return factory->releaseBuild; });
    entry->aliases = *source;
    entry->building = false;
    lock.unlock();
    entry->ready.notify_all();
    return entry->aliases;
}

int ModernEntry(FakeFactory* factory)
{
    std::shared_ptr<int> source = directwrite_readiness::EstablishSource(
        factory,
        std::shared_ptr<int>(),
        PublishedSource,
        [](FakeFactory*, std::shared_ptr<int>& acquired) {
            acquired = std::make_shared<int>(77);
            return true;
        },
        PublishSource);
    return source ? BuildAliases(factory, source) : -1;
}

int FallbackMapping(FakeFactory* factory, bool buildingThread)
{
    std::unique_lock<std::mutex> lock(factory->cacheMutex);
    std::shared_ptr<FakeEntry> entry = factory->entry;
    if (!entry)
        return -1;
    ++factory->waiters;
    factory->stateChanged.notify_all();
    bool const ready = directwrite_readiness::WaitForBuild(
        entry, lock, 10s, !buildingThread);
    return ready ? entry->aliases : -1;
}

enum class ReservationStatus
{
    applied,
    unexpectedFailure,
    systemSetUnavailable,
    waitTimeout,
};

struct ReservationResult
{
    ReservationStatus status = ReservationStatus::systemSetUnavailable;
    int aliases = 0;
};

struct ReservedFakeEntry
{
    std::condition_variable ready;
    std::thread::id reservingThread;
    ReservationStatus status = ReservationStatus::unexpectedFailure;
    int aliases = 0;
    bool building = true;
};

struct ReservedFakeFactory
{
    std::mutex mutex;
    std::condition_variable stateChanged;
    std::shared_ptr<ReservedFakeEntry> entry;
    unsigned int waiters = 0;
    bool fallbackExposed = false;
};

// Models directwrite_alias::BuildReservation around the production cache.
class FakeBuildReservation
{
public:
    explicit FakeBuildReservation(ReservedFakeFactory* factory)
        : factory_(factory)
    {
        std::lock_guard<std::mutex> lock(factory_->mutex);
        if (factory_->entry)
            return;
        entry_ = std::make_shared<ReservedFakeEntry>();
        entry_->reservingThread = std::this_thread::get_id();
        factory_->entry = entry_;
    }

    ~FakeBuildReservation()
    {
        if (!entry_)
            return;
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(factory_->mutex);
            if (entry_->building &&
                entry_->reservingThread == std::this_thread::get_id())
            {
                entry_->status = ReservationStatus::unexpectedFailure;
                entry_->building = false;
                entry_->reservingThread = {};
                if (factory_->entry == entry_)
                    factory_->entry.reset();
                notify = true;
            }
        }
        if (notify)
            entry_->ready.notify_all();
    }

    bool reserved() const noexcept { return entry_ != nullptr; }

private:
    ReservedFakeFactory* factory_;
    std::shared_ptr<ReservedFakeEntry> entry_;
};

// Models HookDirectWriteSystemFallback making the shared vtable slots live.
void ExposeFallbackSlots(ReservedFakeFactory& factory)
{
    std::lock_guard<std::mutex> lock(factory.mutex);
    factory.fallbackExposed = true;
}

// Models directwrite_alias::GetCachedForFactory, including own-reservation
// refusal and the caller-supplied deadline.
ReservationResult ReservedFallbackLookup(
    ReservedFakeFactory& factory,
    std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(factory.mutex);
    std::shared_ptr<ReservedFakeEntry> entry = factory.entry;
    if (!factory.fallbackExposed || !entry)
        return {};
    bool const mayWait =
        entry->reservingThread != std::this_thread::get_id();
    if (entry->building && mayWait)
    {
        ++factory.waiters;
        factory.stateChanged.notify_all();
    }
    std::chrono::steady_clock::duration remaining =
        deadline - std::chrono::steady_clock::now();
    if (remaining < std::chrono::steady_clock::duration::zero())
        remaining = std::chrono::steady_clock::duration::zero();
    if (!directwrite_readiness::WaitForBuild(
            entry, lock, remaining, mayWait))
        return {mayWait ? ReservationStatus::waitTimeout :
            ReservationStatus::systemSetUnavailable, 0};
    return {entry->status, entry->aliases};
}

// Models directwrite_alias::GetOrCreate adopting its reserving thread's entry.
int BuildReservedAliases(ReservedFakeFactory& factory)
{
    std::shared_ptr<ReservedFakeEntry> entry;
    {
        std::lock_guard<std::mutex> lock(factory.mutex);
        entry = factory.entry;
        if (!entry || !entry->building ||
            entry->reservingThread != std::this_thread::get_id())
            return -1;
        entry->reservingThread = {};
        entry->aliases = 77;
        entry->status = ReservationStatus::applied;
        entry->building = false;
    }
    entry->ready.notify_all();
    return entry->aliases;
}

void WaitForReservedWaiter(ReservedFakeFactory& factory)
{
    std::unique_lock<std::mutex> lock(factory.mutex);
    Require(factory.stateChanged.wait_for(
        lock, 10s, [&factory] { return factory.waiters != 0; }),
        "the reserved fallback reader did not reach the readiness barrier");
}

void WaitForBuild(FakeFactory& factory)
{
    std::unique_lock<std::mutex> lock(factory.cacheMutex);
    Require(factory.stateChanged.wait_for(
        lock, 10s, [&factory] { return factory.buildEntered; }),
        "the fake alias build did not start");
}

void WaitForWaiter(FakeFactory& factory)
{
    std::unique_lock<std::mutex> lock(factory.cacheMutex);
    Require(factory.stateChanged.wait_for(
        lock, 10s, [&factory] { return factory.waiters != 0; }),
        "the concurrent reader did not reach the readiness barrier");
}

void ReleaseBuild(FakeFactory& factory)
{
    {
        std::lock_guard<std::mutex> lock(factory.cacheMutex);
        factory.releaseBuild = true;
    }
    factory.stateChanged.notify_all();
}

enum class RetryLookup
{
    cached,
    waiting,
    rebuilt,
};

struct RetryEntry
{
    std::chrono::steady_clock::time_point retryAfter{};
    unsigned failedBuilds = 1;
    unsigned reservingThread = 17;
    bool status = false;
    bool building = false;
    bool retryAllowed = true;
};

RetryLookup LookupFailedEntry(
    RetryEntry& entry,
    std::chrono::steady_clock::time_point now,
    unsigned& buildCount)
{
    if (entry.building)
        return RetryLookup::waiting;
    auto const disposition = directwrite_readiness::PrepareCompletedBuild(
        entry, now, [](bool applied) { return applied; });
    if (disposition ==
        directwrite_readiness::CompletedBuildDisposition::useCached)
        return RetryLookup::cached;
    ++buildCount;
    return RetryLookup::rebuilt;
}

void FinishFailedBuild(
    RetryEntry& entry,
    std::chrono::steady_clock::time_point now)
{
    entry.building = false;
    ++entry.failedBuilds;
    std::optional<std::chrono::milliseconds> const delay =
        directwrite_readiness::FailureRetryDelay(entry.failedBuilds);
    entry.retryAllowed = delay.has_value();
    if (delay)
        entry.retryAfter = now + *delay;
}

void TestFailureRetrySchedule()
{
    using directwrite_readiness::FailureRetryDelay;
    Require(FailureRetryDelay(0) == std::chrono::milliseconds(2s),
        "failure zero did not use the first retry delay");
    Require(FailureRetryDelay(1) == std::chrono::milliseconds(2s),
        "the first failure did not retry after two seconds");
    Require(FailureRetryDelay(2) == std::chrono::milliseconds(15s),
        "the second failure did not retry after fifteen seconds");
    Require(FailureRetryDelay(3) == std::chrono::milliseconds(120s),
        "the third failure did not retry after two minutes");
    Require(!FailureRetryDelay(4),
        "the fourth failure still allowed a retry");
    Require(!FailureRetryDelay(99),
        "a later failure still allowed a retry");
}

void TestFailedBuildRetryPolicy()
{
    auto const started = std::chrono::steady_clock::time_point{};
    RetryEntry entry;
    entry.retryAfter = started + 2s;
    unsigned buildCount = 1;

    Require(LookupFailedEntry(entry, started + 1999ms, buildCount) ==
        RetryLookup::cached,
        "a failed entry rebuilt before its retry time");
    Require(buildCount == 1 && !entry.building,
        "a cached failure changed the build state");

    Require(LookupFailedEntry(entry, started + 2s, buildCount) ==
        RetryLookup::rebuilt,
        "a failed entry was not rebuilt when its retry became due");
    Require(buildCount == 2 && entry.building && entry.reservingThread == 0,
        "the due retry did not take ownership of the completed entry");
    Require(LookupFailedEntry(entry, started + 2s, buildCount) ==
        RetryLookup::waiting && buildCount == 2,
        "two callers rebuilt the same due retry");

    FinishFailedBuild(entry, started + 2s);
    Require(LookupFailedEntry(entry, started + 16s, buildCount) ==
        RetryLookup::cached && buildCount == 2,
        "the second failure ignored its fifteen-second delay");
    Require(LookupFailedEntry(entry, started + 17s, buildCount) ==
        RetryLookup::rebuilt && buildCount == 3,
        "the second retry did not run exactly once when due");

    FinishFailedBuild(entry, started + 17s);
    Require(LookupFailedEntry(entry, started + 137s, buildCount) ==
        RetryLookup::rebuilt && buildCount == 4,
        "the third retry did not run after its two-minute delay");
    FinishFailedBuild(entry, started + 137s);
    Require(!entry.retryAllowed,
        "the fourth failed build did not exhaust the retry schedule");
    Require(LookupFailedEntry(entry, started + 24h, buildCount) ==
        RetryLookup::cached && buildCount == 4,
        "an exhausted failed entry rebuilt again");
}

struct SourceEntry
{
    int factory = 0;
    int generation = 0;
    bool applied = false;
};

void TestNewestMatchingEntry()
{
    std::vector<std::shared_ptr<SourceEntry>> entries = {
        std::make_shared<SourceEntry>(SourceEntry{7, 11, false}),
        std::make_shared<SourceEntry>(SourceEntry{8, 11, true}),
        std::make_shared<SourceEntry>(SourceEntry{7, 11, true}),
    };
    std::shared_ptr<SourceEntry> const newest =
        directwrite_readiness::NewestMatching(
            entries, [](std::shared_ptr<SourceEntry> const& candidate) {
                return candidate->factory == 7 && candidate->generation == 11;
            });
    Require(newest == entries.back() && newest->applied,
        "fallback lookup selected an older entry for the same generation");
    Require(!directwrite_readiness::NewestMatching(
            entries, [](std::shared_ptr<SourceEntry> const& candidate) {
                return candidate->factory == 9;
            }),
        "a missing cache match did not return a value-initialized result");
}

void TestModernEntryWaits()
{
    FakeFactory factory;
    int first = -1;
    int second = -1;
    std::thread builder([&] { first = ModernEntry(&factory); });
    WaitForBuild(factory);
    std::thread modern([&] { second = ModernEntry(&factory); });
    WaitForWaiter(factory);
    ReleaseBuild(factory);
    builder.join();
    modern.join();
    Require(first == 77 && second == 77,
        "a modern collection call retained stock state during the cold build");
}

void TestFallbackWaits()
{
    FakeFactory factory;
    int built = -1;
    int mapped = -1;
    std::thread builder([&] { built = ModernEntry(&factory); });
    WaitForBuild(factory);
    Require(FallbackMapping(&factory, true) == -1,
        "the building thread waited on its own fallback lookup");
    {
        std::lock_guard<std::mutex> lock(factory.cacheMutex);
        factory.waiters = 0;
    }
    std::thread fallback([&] { mapped = FallbackMapping(&factory, false); });
    WaitForWaiter(factory);
    ReleaseBuild(factory);
    builder.join();
    fallback.join();
    Require(built == 77 && mapped == 77,
        "fallback mapping retained the native font during the cold build");
}

void TestReservedFallbackWaits()
{
    ReservedFakeFactory factory;
    ReservationResult mapped;
    FakeBuildReservation const reservation(&factory);
    Require(reservation.reserved(), "the fake build entry was not reserved");
    ExposeFallbackSlots(factory);
    std::thread fallback([&] {
        mapped = ReservedFallbackLookup(
            factory, std::chrono::steady_clock::now() + 10s);
    });
    WaitForReservedWaiter(factory);
    int const built = BuildReservedAliases(factory);
    fallback.join();
    Require(built == 77 && mapped.status == ReservationStatus::applied &&
        mapped.aliases == 77,
        "a fallback lookup escaped between slot exposure and the alias build");
}

void TestOwnReservationDoesNotWait()
{
    ReservedFakeFactory factory;
    FakeBuildReservation const reservation(&factory);
    Require(reservation.reserved(), "the own-wait fake entry was not reserved");
    ExposeFallbackSlots(factory);
    ReservationResult const result = ReservedFallbackLookup(
        factory, std::chrono::steady_clock::now() + 200ms);
    Require(result.status == ReservationStatus::systemSetUnavailable,
        "the reserving thread waited on its own build entry");
}

void TestAbandonedReservationReleasesWaiter()
{
    ReservedFakeFactory factory;
    ReservationResult mapped;
    std::thread fallback;
    {
        FakeBuildReservation const reservation(&factory);
        Require(reservation.reserved(), "the abandoned fake entry was not reserved");
        ExposeFallbackSlots(factory);
        fallback = std::thread([&] {
            mapped = ReservedFallbackLookup(
                factory, std::chrono::steady_clock::now() + 10s);
        });
        WaitForReservedWaiter(factory);
    }
    fallback.join();
    Require(mapped.status == ReservationStatus::unexpectedFailure,
        "an abandoned reservation left its fallback lookup waiting");
}

void TestFallbackSourcesShareDeadline()
{
    std::mutex mutex;
    std::shared_ptr<ReservedFakeEntry> first =
        std::make_shared<ReservedFakeEntry>();
    std::shared_ptr<ReservedFakeEntry> second =
        std::make_shared<ReservedFakeEntry>();
    auto const started = std::chrono::steady_clock::now();
    auto const timeout = 200ms;
    auto const deadline = started + timeout;
    unsigned int timeouts = 0;
    for (std::shared_ptr<ReservedFakeEntry> const& entry : {first, second})
    {
        std::unique_lock<std::mutex> lock(mutex);
        std::chrono::steady_clock::duration remaining =
            deadline - std::chrono::steady_clock::now();
        if (remaining < std::chrono::steady_clock::duration::zero())
            remaining = std::chrono::steady_clock::duration::zero();
        if (!directwrite_readiness::WaitForBuild(
                entry, lock, remaining, true))
            ++timeouts;
    }
    auto const elapsed = std::chrono::steady_clock::now() - started;
    Require(timeouts == 2, "the stalled fallback sources did not time out");
    Require(elapsed < timeout * 3 / 2,
        "fallback sources each consumed a full build timeout");
}

void TestNestedScopeIsRefused()
{
    using directwrite_readiness::CanonicalSourceScope;
    {
        CanonicalSourceScope const outer;
        Require(!outer.nested(), "the first scope on a thread reported itself nested");
        {
            CanonicalSourceScope const inner;
            Require(inner.nested(),
                "DirectWrite's re-entrant call on the establishing thread was not refused");
        }
        CanonicalSourceScope const again;
        Require(again.nested(), "an inner scope ending released the outer scope's mark");
        bool otherThreadNested = true;
        std::thread other([&] {
            CanonicalSourceScope const theirs;
            otherThreadNested = theirs.nested();
        });
        other.join();
        Require(!otherThreadNested, "the establishing mark leaked to another thread");
    }
    CanonicalSourceScope const fresh;
    Require(!fresh.nested(), "the outer scope ending did not release the mark");
}

} // namespace

int main()
{
    TestFailureRetrySchedule();
    TestFailedBuildRetryPolicy();
    TestNewestMatchingEntry();
    TestModernEntryWaits();
    TestFallbackWaits();
    TestReservedFallbackWaits();
    TestOwnReservationDoesNotWait();
    TestAbandonedReservationReleasesWaiter();
    TestFallbackSourcesShareDeadline();
    TestNestedScopeIsRefused();
    std::cout << "DirectWrite readiness tests passed.\n";
    return 0;
}
