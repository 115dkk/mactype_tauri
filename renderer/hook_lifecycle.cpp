#include "hook_lifecycle.h"

#include <algorithm>

namespace renderer {

namespace {

bool IsUnavailable(CapabilityReason reason) noexcept
{
    return reason == CapabilityReason::moduleMissing ||
           reason == CapabilityReason::interfaceUnsupported ||
           reason == CapabilityReason::explicitlyDisabled ||
           reason == CapabilityReason::antiCheatDetected ||
           reason == CapabilityReason::safetyEvidenceUnavailable;
}

} // namespace

bool HookCoordinator::BeginStart() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RuntimePhase::uninitialized && phase_ != RuntimePhase::failed) {
        return false;
    }
    phase_ = RuntimePhase::starting;
    failureStatus_ = 0;
    ++generation_;
    ++revision_;
    capabilities_.clear();
    return true;
}

bool HookCoordinator::CompleteStart() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RuntimePhase::starting) {
        return false;
    }
    phase_ = RuntimePhase::active;
    ++revision_;
    return true;
}

bool HookCoordinator::FailStart(long status) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RuntimePhase::starting) {
        return false;
    }
    phase_ = RuntimePhase::failed;
    failureStatus_ = status;
    ++revision_;
    return true;
}

bool HookCoordinator::BeginStop() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RuntimePhase::starting && phase_ != RuntimePhase::active &&
        phase_ != RuntimePhase::failed) {
        return false;
    }
    phaseBeforeStop_ = phase_;
    phase_ = RuntimePhase::stopping;
    ++revision_;
    return true;
}

bool HookCoordinator::AbortStop() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RuntimePhase::stopping) {
        return false;
    }
    phase_ = phaseBeforeStop_;
    phaseBeforeStop_ = RuntimePhase::uninitialized;
    ++revision_;
    return true;
}

bool HookCoordinator::CompleteStop() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RuntimePhase::stopping) {
        return false;
    }
    if (std::any_of(capabilities_.begin(), capabilities_.end(), [](const CapabilityEntry& entry) {
            return entry.record.state == CapabilityState::pending;
        })) {
        return false;
    }
    for (CapabilityEntry& entry : capabilities_) {
        if (entry.record.state == CapabilityState::active) {
            entry.record.state = CapabilityState::stopped;
        }
    }
    phase_ = RuntimePhase::uninitialized;
    phaseBeforeStop_ = RuntimePhase::uninitialized;
    ++revision_;
    return true;
}

bool HookCoordinator::ClearForQuietUnload() noexcept
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (phase_ != RuntimePhase::active ||
		std::any_of(
			capabilities_.begin(), capabilities_.end(),
			[](const CapabilityEntry& entry) {
				return entry.record.state == CapabilityState::pending ||
					entry.record.state == CapabilityState::active ||
					entry.record.state == CapabilityState::failed;
			}))
		return false;
	std::vector<CapabilityEntry>().swap(capabilities_);
	phase_ = RuntimePhase::uninitialized;
	phaseBeforeStop_ = RuntimePhase::uninitialized;
	failureStatus_ = 0;
	nextAttemptSequence_ = 1;
	++revision_;
	return true;
}

HookAttempt HookCoordinator::BeginAttempt(
    HookCapability capability,
    std::uintptr_t target,
    bool modulePresent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RuntimePhase::starting && phase_ != RuntimePhase::active) {
        return {};
    }

    const auto matchesTarget = [capability, target](const CapabilityEntry& entry) {
        return entry.record.capability == capability && entry.record.target == target;
    };
    auto entry = std::find_if(capabilities_.begin(), capabilities_.end(), matchesTarget);
    if (entry != capabilities_.end() &&
        (entry->record.state == CapabilityState::pending ||
         entry->record.state == CapabilityState::active)) {
        return {};
    }

    if (entry == capabilities_.end()) {
        CapabilityEntry newEntry;
        newEntry.record.capability = capability;
        newEntry.record.target = target;
        capabilities_.push_back(newEntry);
        entry = capabilities_.end() - 1;
    }

    const std::uint64_t sequence = nextAttemptSequence_++;
    entry->record.state = CapabilityState::pending;
    entry->record.reason = CapabilityReason::none;
    entry->record.modulePresent = modulePresent;
    ++entry->record.attempts;
    entry->record.status = 0;
    entry->pendingSequence = sequence;
    ++revision_;

    HookAttempt attempt;
    attempt.capability_ = capability;
    attempt.target_ = target;
    attempt.generation_ = generation_;
    attempt.sequence_ = sequence;
    return attempt;
}

bool HookCoordinator::CompleteAttempt(
    const HookAttempt& attempt,
    bool succeeded,
    CapabilityReason reason,
    long status) noexcept
{
    if (!attempt.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (attempt.generation_ != generation_) {
        return false;
    }

    const auto matchesAttempt = [&attempt](const CapabilityEntry& entry) {
        return entry.record.capability == attempt.capability_ &&
               entry.record.target == attempt.target_ &&
               entry.pendingSequence == attempt.sequence_;
    };
    const auto entry = std::find_if(capabilities_.begin(), capabilities_.end(), matchesAttempt);
    if (entry == capabilities_.end() || entry->record.state != CapabilityState::pending) {
        return false;
    }

    entry->record.state = succeeded
                              ? CapabilityState::active
                              : (IsUnavailable(reason) ? CapabilityState::unavailable
                                                       : CapabilityState::failed);
    entry->record.reason = succeeded ? CapabilityReason::none : reason;
    entry->record.status = succeeded ? 0 : status;
    if (!succeeded && !entry->hasFailureEvidence) {
        entry->record.firstFailureReason = reason;
        entry->record.firstFailureStatus = status;
        entry->hasFailureEvidence = true;
    }
    entry->pendingSequence = 0;
    ++revision_;
    return true;
}

HookLifecycleSnapshot HookCoordinator::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    HookLifecycleSnapshot snapshot;
    snapshot.phase = phase_;
    snapshot.failureStatus = failureStatus_;
    snapshot.generation = generation_;
    snapshot.revision = revision_;
    snapshot.capabilities.reserve(capabilities_.size());
    for (const CapabilityEntry& entry : capabilities_) {
        snapshot.capabilities.push_back(entry.record);
    }
    return snapshot;
}

RuntimePhase HookCoordinator::phase() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return phase_;
}

long HookCoordinator::failure_status() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return failureStatus_;
}

HookCoordinator& ProcessHookCoordinator()
{
    static HookCoordinator* coordinator = new HookCoordinator;
    return *coordinator;
}

} // namespace renderer
