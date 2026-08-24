#include "../../../renderer/unload_lifecycle.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

struct FakeProvider final
{
    renderer::UnloadProviderPreparation preparation =
        renderer::UnloadProviderPreparation::prepared;
    unsigned int prepares = 0;
    unsigned int aborts = 0;
    unsigned int commits = 0;
    bool commitResult = true;
};

renderer::UnloadProviderPreparation PrepareFake(
    void* context,
    DWORD) noexcept
{
    auto& fake = *static_cast<FakeProvider*>(context);
    ++fake.prepares;
    return fake.preparation;
}

void AbortFake(void* context) noexcept
{
    ++static_cast<FakeProvider*>(context)->aborts;
}

bool CommitFake(void* context, DWORD) noexcept
{
    ++static_cast<FakeProvider*>(context)->commits;
    return static_cast<FakeProvider*>(context)->commitResult;
}

renderer::UnloadProviderAdapter Adapter(FakeProvider& fake)
{
    return {&fake, PrepareFake, AbortFake, CommitFake};
}

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    FakeProvider first;
    FakeProvider unsafe;
    unsafe.preparation = renderer::UnloadProviderPreparation::unsafeToUnload;
    std::array<renderer::UnloadProviderAdapter, 2> providers{
        Adapter(first), Adapter(unsafe)};
    {
        renderer::RendererProviderDrainTransaction drain(
            providers.data(), providers.size());
        Require(!drain.Prepare(),
            "an unsafe provider must reject the whole renderer drain");
    }
    Require(first.prepares == 1 && first.aborts == 1 && first.commits == 0 &&
        unsafe.prepares == 1 && unsafe.aborts == 0 && unsafe.commits == 0,
        "provider preparation rollback did not preserve transactional order");

    FakeProvider commitFailure;
    commitFailure.commitResult = false;
    FakeProvider notYetCommitted;
    providers = {Adapter(commitFailure), Adapter(notYetCommitted)};
    {
        renderer::RendererProviderDrainTransaction drain(
            providers.data(), providers.size());
        Require(drain.Prepare() && !drain.Commit(),
            "a provider commit failure must remain retryable");
    }
    Require(commitFailure.commits == 1 && commitFailure.aborts == 0 &&
        notYetCommitted.commits == 0 && notYetCommitted.aborts == 1,
        "commit failure did not retain the partial provider and abort later work");

    FakeProvider abandonedFirst;
    FakeProvider abandonedSecond;
    providers = {Adapter(abandonedFirst), Adapter(abandonedSecond)};
    {
        renderer::RendererProviderDrainTransaction drain(
            providers.data(), providers.size());
        Require(drain.Prepare(), "valid providers did not prepare");
    }
    Require(abandonedFirst.aborts == 1 && abandonedSecond.aborts == 1,
        "an abandoned prepared drain did not roll back every provider");

    renderer::UnloadAttemptGate gate;
    Require(gate.TryBegin(), "the first unload attempt must acquire admission");
    Require(gate.active(), "an acquired unload attempt must be observable");
    Require(!gate.TryBegin(), "a concurrent unload attempt must be rejected");
    gate.EndForRetry();
    Require(!gate.active(), "a failed unload must publish retry admission");
    Require(gate.TryBegin(), "a later unload attempt must be able to retry");
    gate.EndForRetry();

    std::atomic<bool> start{false};
    std::atomic<unsigned int> winners{0};
    std::vector<std::thread> threads;
    for (unsigned int index = 0; index < 16; ++index) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (gate.TryBegin()) {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    Require(winners.load(std::memory_order_relaxed) == 1,
            "exactly one racing unload caller must acquire admission");
    gate.EndForRetry();

    HMODULE const kernel32 = GetModuleHandleW(L"kernel32.dll");
    Require(renderer::AcquireProcessRendererLease(kernel32),
            "a mapped image must be able to acquire an owned module lease");
    Require(renderer::ProcessRendererLeaseHeld(),
            "the process lease must remain published until safe unload takes it");
    HMODULE const lease = renderer::TakeProcessRendererLease();
    Require(lease == kernel32 && !renderer::ProcessRendererLeaseHeld(),
            "taking the process lease must atomically close plain detach admission");
    Require(FreeLibrary(lease) != FALSE,
            "the focused test must release the extra system-module reference");

    std::cout << "Unload lifecycle tests passed.\n";
    return 0;
}
