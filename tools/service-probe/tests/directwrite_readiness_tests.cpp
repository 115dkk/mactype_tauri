#include "../../../renderer/directwrite_readiness.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

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
    TestModernEntryWaits();
    TestFallbackWaits();
    TestNestedScopeIsRefused();
    std::cout << "DirectWrite readiness tests passed.\n";
    return 0;
}
