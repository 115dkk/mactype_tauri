#include "../../../renderer/unload_lifecycle.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

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
