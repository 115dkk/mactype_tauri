#include "../../../renderer/hook_lifecycle.h"

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
    renderer::HookCoordinator coordinator;

    Require(coordinator.phase() == renderer::RuntimePhase::uninitialized,
            "a new renderer runtime must be uninitialized");
    Require(coordinator.BeginStart(), "the first start must own initialization");
    Require(!coordinator.BeginStart(), "a concurrent start must be idempotently rejected");
    Require(coordinator.CompleteStart(), "the owner must publish an active runtime");
    Require(coordinator.phase() == renderer::RuntimePhase::active,
            "completed initialization must publish active");
    Require(coordinator.BeginStop(), "the first stop must block new lifecycle work");
    Require(!coordinator.BeginStop(), "a repeated stop must be idempotently rejected");
    Require(coordinator.CompleteStop(), "a drained stop must return to uninitialized");
    Require(coordinator.phase() == renderer::RuntimePhase::uninitialized,
            "a stopped renderer must be restartable from uninitialized");

    renderer::HookCoordinator failed;
    Require(failed.BeginStart(), "failure test must enter starting");
    Require(failed.FailStart(1234), "the start owner must publish its first failure");
    Require(failed.phase() == renderer::RuntimePhase::failed,
            "failed initialization must remain observable");
    Require(failed.failure_status() == 1234,
            "failed initialization must preserve its first meaningful status");
    Require(failed.BeginStart(), "an explicitly retried failed runtime must be restartable");
    Require(failed.failure_status() == 0,
            "a new generation must not inherit the prior failure status");

    renderer::HookCoordinator capabilities;
    Require(capabilities.BeginStart(), "capability registry must start with the runtime");
    renderer::HookAttempt first = capabilities.BeginAttempt(
        renderer::HookCapability::directWrite, 0x1000, true);
    Require(first.valid(), "the first concrete hook target must be attempted");
    Require(!capabilities.BeginAttempt(
                 renderer::HookCapability::directWrite, 0x1000, true).valid(),
            "the same hook target must not be patched twice while pending");
    Require(capabilities.CompleteAttempt(first, true, renderer::CapabilityReason::none, 0),
            "the owner must publish the hook result");
    const renderer::HookLifecycleSnapshot snapshot = capabilities.Snapshot();
    Require(snapshot.capabilities.size() == 1,
            "one concrete target must produce one capability record");
    Require(snapshot.capabilities[0].state == renderer::CapabilityState::active,
            "a successful target must publish an active capability");
    Require(snapshot.capabilities[0].attempts == 1,
            "duplicate suppression must not inflate the attempt count");
    Require(snapshot.capabilities[0].modulePresent,
            "the registry must preserve whether the provider module was present");

    renderer::HookCoordinator retries;
    Require(retries.BeginStart(), "retry registry must start with the runtime");
    const renderer::HookAttempt missing = retries.BeginAttempt(
        renderer::HookCapability::directWriteCore, 0x2000, false);
    Require(retries.CompleteAttempt(
                missing, false, renderer::CapabilityReason::moduleMissing, 126),
            "an unavailable module must publish a deterministic result");
    renderer::HookLifecycleSnapshot retrySnapshot = retries.Snapshot();
    Require(retrySnapshot.capabilities[0].state == renderer::CapabilityState::unavailable,
            "an absent optional provider is unavailable rather than a runtime failure");
    Require(retrySnapshot.capabilities[0].firstFailureReason ==
                renderer::CapabilityReason::moduleMissing,
            "the first meaningful failure reason must remain diagnostic evidence");
    Require(retrySnapshot.capabilities[0].firstFailureStatus == 126,
            "the first meaningful failure status must remain diagnostic evidence");
    const renderer::HookAttempt retry = retries.BeginAttempt(
        renderer::HookCapability::directWriteCore, 0x2000, true);
    Require(retry.valid(), "an unavailable target must be retryable when its module appears");
    Require(retries.CompleteAttempt(retry, true, renderer::CapabilityReason::none, 0),
            "a later successful retry must activate the capability");
    retrySnapshot = retries.Snapshot();
    Require(retrySnapshot.capabilities[0].state == renderer::CapabilityState::active,
            "a successful retry must replace the current unavailable state");
    Require(retrySnapshot.capabilities[0].attempts == 2,
            "the registry must count concrete retry attempts exactly");
    Require(retrySnapshot.capabilities[0].firstFailureStatus == 126,
            "successful recovery must not erase the first failure evidence");

    renderer::HookCoordinator draining;
    Require(draining.BeginStart(), "drain registry must start with the runtime");
    const renderer::HookAttempt pending = draining.BeginAttempt(
        renderer::HookCapability::gdi, 0x3000, true);
    Require(draining.BeginStop(), "stop must close the lifecycle admission gate");
    Require(!draining.BeginAttempt(
                 renderer::HookCapability::direct2D, 0x4000, true).valid(),
            "stopping must reject new hook transactions");
    Require(!draining.CompleteStop(), "stop must not complete while a patch is pending");
    Require(draining.CompleteAttempt(pending, true, renderer::CapabilityReason::none, 0),
            "an admitted patch must be allowed to drain while stopping");
    Require(draining.CompleteStop(), "a fully drained runtime must complete its stop");
    const renderer::HookLifecycleSnapshot stoppedSnapshot = draining.Snapshot();
    Require(stoppedSnapshot.capabilities[0].state == renderer::CapabilityState::stopped,
            "stopped capabilities must remain observable until the next generation");

    renderer::HookCoordinator abortedStop;
    Require(abortedStop.BeginStart(), "abort registry must start with the runtime");
    Require(abortedStop.CompleteStart(), "abort registry must become active");
    Require(abortedStop.BeginStop(), "abort registry must begin stopping");
    Require(abortedStop.AbortStop(), "a failed unload must reopen lifecycle admission");
    Require(abortedStop.phase() == renderer::RuntimePhase::active,
            "an aborted stop must restore the prior active phase");

    renderer::HookCoordinator quietUnload;
    Require(quietUnload.BeginStart(), "quiet unload registry must start");
    const renderer::HookAttempt unavailable = quietUnload.BeginAttempt(
        renderer::HookCapability::gdi, 0, true);
    Require(quietUnload.CompleteAttempt(
                unavailable, false, renderer::CapabilityReason::explicitlyDisabled, 0),
            "quiet unload must record an explicitly unavailable capability");
    Require(quietUnload.CompleteStart(), "quiet unload registry must become active");
    Require(quietUnload.ClearForQuietUnload(),
            "a runtime with no active or failed hook may release quiet state");
    Require(quietUnload.phase() == renderer::RuntimePhase::uninitialized,
            "quiet state release must close the lifecycle generation");
    Require(quietUnload.Snapshot().capabilities.empty(),
            "quiet state release must free capability storage before DLL unload");

    Require(capabilities.CompleteStart(), "active capability registry must become active");
    Require(!capabilities.ClearForQuietUnload(),
            "an active hook must block the quiet-unload cleanup path");

    renderer::HookCoordinator concurrent;
    Require(concurrent.BeginStart(), "concurrency registry must start with the runtime");
    std::atomic<unsigned int> admitted{0};
    std::vector<std::thread> contenders;
    for (unsigned int index = 0; index < 16; ++index) {
        contenders.emplace_back([&concurrent, &admitted]() {
            if (concurrent.BeginAttempt(
                    renderer::HookCapability::directWrite, 0x5000, true).valid()) {
                ++admitted;
            }
        });
    }
    for (std::thread& contender : contenders) {
        contender.join();
    }
    Require(admitted == 1,
            "concurrent discovery of one concrete target must admit exactly one patch");

    std::cout << "Hook lifecycle state tests passed.\n";
    return 0;
}
