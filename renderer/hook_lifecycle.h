#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace renderer {

enum class RuntimePhase : unsigned char
{
    uninitialized,
    starting,
    active,
    failed,
    stopping,
};

enum class HookCapability : unsigned char
{
    gdi,
    childInjection,
    directWrite,
    directWriteCore,
    direct2D,
    gdiPlus,
    fontSubstitution,
};

enum class CapabilityState : unsigned char
{
    unknown,
    pending,
    active,
    unavailable,
    failed,
    stopped,
};

enum class CapabilityReason : unsigned char
{
    none,
    moduleMissing,
    interfaceUnsupported,
    explicitlyDisabled,
    transactionFailed,
    lifecycleStopping,
    initializationFailed,
};

class HookAttempt
{
public:
    [[nodiscard]] bool valid() const noexcept { return sequence_ != 0; }

private:
    friend class HookCoordinator;

    HookCapability capability_ = HookCapability::gdi;
    std::uintptr_t target_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t sequence_ = 0;
};

struct CapabilityRecord
{
    HookCapability capability = HookCapability::gdi;
    std::uintptr_t target = 0;
    CapabilityState state = CapabilityState::unknown;
    CapabilityReason reason = CapabilityReason::none;
    bool modulePresent = false;
    unsigned int attempts = 0;
    long status = 0;
    CapabilityReason firstFailureReason = CapabilityReason::none;
    long firstFailureStatus = 0;
};

struct HookLifecycleSnapshot
{
    RuntimePhase phase = RuntimePhase::uninitialized;
    long failureStatus = 0;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::vector<CapabilityRecord> capabilities;
};

class HookCoordinator
{
public:
    bool BeginStart() noexcept;
    bool CompleteStart() noexcept;
    bool FailStart(long status) noexcept;
    bool BeginStop() noexcept;
    bool AbortStop() noexcept;
    bool CompleteStop() noexcept;
    [[nodiscard]] HookAttempt BeginAttempt(
        HookCapability capability,
        std::uintptr_t target,
        bool modulePresent);
    bool CompleteAttempt(
        const HookAttempt& attempt,
        bool succeeded,
        CapabilityReason reason,
        long status) noexcept;
    [[nodiscard]] HookLifecycleSnapshot Snapshot() const;
    RuntimePhase phase() const noexcept;
    long failure_status() const noexcept;

private:
    struct CapabilityEntry
    {
        CapabilityRecord record;
        std::uint64_t pendingSequence = 0;
        bool hasFailureEvidence = false;
    };

    mutable std::mutex mutex_;
    RuntimePhase phase_ = RuntimePhase::uninitialized;
    RuntimePhase phaseBeforeStop_ = RuntimePhase::uninitialized;
    long failureStatus_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t nextAttemptSequence_ = 1;
    std::vector<CapabilityEntry> capabilities_;
};

// Process-owned hook state intentionally outlives DLL static destruction. The
// explicit unload path drains it before releasing the renderer image.
HookCoordinator& ProcessHookCoordinator();

} // namespace renderer
