use std::collections::VecDeque;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use mactype_service_contract::{
    ConsoleProcessPolicy, PrivateFreeTypePolicy, ProfileDigest, RendererRuntimeBinding,
    RuntimeGenerationId, UnityFontHookPolicy,
};
use mactype_service_host::{
    BinarySignaturePolicy, BrokerDisposition, BrokerResult, DeferralReason, DynamicCodePolicy,
    ImageSubsystem, InjectionBroker, InjectionRequest, InspectionEvidence, ProcessArchitecture,
    ProcessIdentity, ProcessInspection, ProcessInspectionError, ProcessInspector,
    ProcessOrchestrator, ProcessOutcome, SessionChange, TargetLifecycle, TargetLiveness,
    MAX_DEFERRED_TARGETS, TARGET_VANISHED_RESULT_CODE,
};

fn binding() -> RendererRuntimeBinding {
    RendererRuntimeBinding::new(
        RuntimeGenerationId::parse(&"a".repeat(64)).unwrap(),
        ProfileDigest::parse(&format!("sha256:{}", "b".repeat(64))).unwrap(),
    )
}

fn identity(pid: u32) -> ProcessIdentity {
    ProcessIdentity {
        pid,
        creation_time: u64::from(pid) + 100,
        session_id: 2,
        architecture: ProcessArchitecture::X64,
    }
}

fn inspection(identity: ProcessIdentity) -> ProcessInspection {
    ProcessInspection {
        identity,
        image_name: InspectionEvidence::Known("ordinary.exe".to_owned()),
        protected: InspectionEvidence::Known(false),
        critical: InspectionEvidence::Known(false),
        dynamic_code: InspectionEvidence::Known(DynamicCodePolicy {
            prohibit_dynamic_code: false,
            allow_thread_opt_out: false,
        }),
        binary_signature: InspectionEvidence::Known(BinarySignaturePolicy {
            microsoft_signed_only: false,
            store_signed_only: false,
            mitigation_opt_in: false,
        }),
    }
}

struct MutableInspector {
    lifecycle: Mutex<TargetLifecycle>,
    liveness: Mutex<TargetLiveness>,
}

impl MutableInspector {
    fn new(lifecycle: TargetLifecycle, liveness: TargetLiveness) -> Self {
        Self {
            lifecycle: Mutex::new(lifecycle),
            liveness: Mutex::new(liveness),
        }
    }

    fn set_lifecycle(&self, lifecycle: TargetLifecycle) {
        *self.lifecycle.lock().unwrap() = lifecycle;
    }
}

impl ProcessInspector for MutableInspector {
    fn inspect(&self, pid: u32) -> Result<ProcessInspection, ProcessInspectionError> {
        Ok(inspection(identity(pid)))
    }

    fn probe_target_lifecycle(&self, _identity: &ProcessIdentity) -> TargetLifecycle {
        *self.lifecycle.lock().unwrap()
    }

    fn probe_target_liveness(&self, _identity: &ProcessIdentity) -> TargetLiveness {
        *self.liveness.lock().unwrap()
    }
}

struct ConsoleGraceInspector {
    identities: Mutex<VecDeque<ProcessIdentity>>,
    subsystem: ImageSubsystem,
    age: Option<Duration>,
    liveness: Mutex<TargetLiveness>,
    liveness_probes: Mutex<usize>,
}

impl ConsoleGraceInspector {
    fn new(
        identities: impl IntoIterator<Item = ProcessIdentity>,
        subsystem: ImageSubsystem,
        age: Option<Duration>,
    ) -> Self {
        Self {
            identities: Mutex::new(identities.into_iter().collect()),
            subsystem,
            age,
            liveness: Mutex::new(TargetLiveness::Alive),
            liveness_probes: Mutex::new(0),
        }
    }

    fn set_liveness(&self, liveness: TargetLiveness) {
        *self.liveness.lock().unwrap() = liveness;
    }
}

impl ProcessInspector for ConsoleGraceInspector {
    fn inspect(&self, pid: u32) -> Result<ProcessInspection, ProcessInspectionError> {
        let identity = self.identities.lock().unwrap().pop_front().unwrap();
        assert_eq!(identity.pid, pid);
        Ok(inspection(identity))
    }

    fn probe_image_subsystem(&self, _identity: &ProcessIdentity) -> ImageSubsystem {
        self.subsystem
    }

    fn probe_process_age(&self, _identity: &ProcessIdentity) -> Option<Duration> {
        self.age
    }

    fn probe_target_liveness(&self, _identity: &ProcessIdentity) -> TargetLiveness {
        *self.liveness_probes.lock().unwrap() += 1;
        *self.liveness.lock().unwrap()
    }
}

struct SequenceBroker {
    results: Mutex<VecDeque<BrokerResult>>,
    requests: Mutex<Vec<InjectionRequest>>,
}

impl SequenceBroker {
    fn new(results: impl IntoIterator<Item = BrokerResult>) -> Self {
        Self {
            results: Mutex::new(results.into_iter().collect()),
            requests: Mutex::new(Vec::new()),
        }
    }

    fn request_count(&self) -> usize {
        self.requests.lock().unwrap().len()
    }
}

impl InjectionBroker for SequenceBroker {
    fn inject(&self, request: &InjectionRequest) -> BrokerResult {
        self.requests.lock().unwrap().push(request.clone());
        self.results.lock().unwrap().pop_front().unwrap()
    }
}

fn broker_result(disposition: BrokerDisposition, code: &str) -> BrokerResult {
    BrokerResult::new(disposition, code, None)
}

#[test]
fn fresh_console_target_waits_only_for_the_remaining_grace_then_injects() {
    let target = identity(42);
    let inspector = ConsoleGraceInspector::new(
        [target.clone(), target.clone(), target.clone()],
        ImageSubsystem::Console,
        Some(Duration::from_millis(750)),
    );
    let broker = SequenceBroker::new([broker_result(
        BrokerDisposition::Injected,
        "renderer-active",
    )]);
    let mut orchestrator = ProcessOrchestrator::with_profile_policies(
        900,
        binding(),
        &inspector,
        &broker,
        UnityFontHookPolicy::default(),
        PrivateFreeTypePolicy::default(),
        ConsoleProcessPolicy::default(),
    );
    let start = Instant::now();

    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Deferred
    );
    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Duplicate
    );
    let deferred = orchestrator.deferred_targets().pop().unwrap();
    assert_eq!(deferred.reason, DeferralReason::ConsoleGrace);
    assert_eq!(deferred.not_before, start + Duration::from_millis(1_250));
    assert_eq!(
        orchestrator.poll_deferred(deferred.not_before).unwrap(),
        Some(ProcessOutcome::Injected)
    );
    assert_eq!(broker.request_count(), 1);
    assert_eq!(*inspector.liveness_probes.lock().unwrap(), 1);
    assert!(orchestrator.deferred_targets().is_empty());
}

#[test]
fn console_grace_does_not_defer_old_non_console_or_unknown_age_targets() {
    for (subsystem, age) in [
        (ImageSubsystem::Console, Some(Duration::from_secs(2))),
        (ImageSubsystem::Gui, Some(Duration::ZERO)),
        (ImageSubsystem::Other, Some(Duration::ZERO)),
        (ImageSubsystem::Unavailable, Some(Duration::ZERO)),
        (ImageSubsystem::Console, None),
    ] {
        let target = identity(42);
        let inspector = ConsoleGraceInspector::new([target], subsystem, age);
        let broker = SequenceBroker::new([broker_result(
            BrokerDisposition::Injected,
            "renderer-active",
        )]);
        let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);

        assert_eq!(
            orchestrator.handle_pid(42).unwrap(),
            ProcessOutcome::Injected,
            "subsystem={subsystem:?} age={age:?}"
        );
        assert_eq!(broker.request_count(), 1);
        assert!(orchestrator.deferred_targets().is_empty());
    }
}

#[test]
fn console_grace_checks_liveness_before_revalidation_and_records_vanished() {
    let target = identity(42);
    let inspector =
        ConsoleGraceInspector::new([target], ImageSubsystem::Console, Some(Duration::ZERO));
    inspector.set_liveness(TargetLiveness::Vanished);
    let broker = SequenceBroker::new([]);
    let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);
    let start = Instant::now();

    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Deferred
    );
    assert_eq!(
        orchestrator
            .poll_deferred(start + Duration::from_secs(2))
            .unwrap(),
        Some(ProcessOutcome::Skipped)
    );
    assert_eq!(broker.request_count(), 0);
    let result = orchestrator.last_result(42, 142).unwrap();
    assert_eq!(result.code, TARGET_VANISHED_RESULT_CODE);
    assert_eq!(result.outcome, ProcessOutcome::Skipped);
}

#[test]
fn changed_identity_during_console_grace_is_recorded_as_vanished_without_injection() {
    let original = identity(42);
    let reused = ProcessIdentity {
        creation_time: original.creation_time + 1,
        ..original.clone()
    };
    let inspector = ConsoleGraceInspector::new(
        [original, reused],
        ImageSubsystem::Console,
        Some(Duration::ZERO),
    );
    let broker = SequenceBroker::new([]);
    let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);
    let start = Instant::now();

    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Deferred
    );
    assert_eq!(
        orchestrator
            .poll_deferred(start + Duration::from_secs(2))
            .unwrap(),
        Some(ProcessOutcome::Skipped)
    );
    assert_eq!(broker.request_count(), 0);
    assert_eq!(
        orchestrator.last_result(42, 142).unwrap().code,
        TARGET_VANISHED_RESULT_CODE
    );
}

#[test]
fn frozen_target_is_deferred_deduplicated_and_injected_after_it_runs() {
    let inspector = MutableInspector::new(TargetLifecycle::Frozen, TargetLiveness::Alive);
    let broker = SequenceBroker::new([broker_result(
        BrokerDisposition::Injected,
        "renderer-active",
    )]);
    let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);
    let start = Instant::now();

    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Deferred
    );
    assert_eq!(orchestrator.deferred_targets().len(), 1);
    assert_eq!(
        orchestrator.deferred_targets()[0].reason,
        DeferralReason::Frozen
    );
    assert_eq!(broker.request_count(), 0);
    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Duplicate
    );
    assert_eq!(
        orchestrator
            .poll_deferred(start + Duration::from_secs(1))
            .unwrap(),
        None
    );

    inspector.set_lifecycle(TargetLifecycle::Running);
    assert_eq!(
        orchestrator
            .poll_deferred(start + Duration::from_secs(2))
            .unwrap(),
        Some(ProcessOutcome::Injected)
    );
    assert_eq!(broker.request_count(), 1);
    assert!(orchestrator.deferred_targets().is_empty());
}

#[test]
fn frozen_target_that_exits_becomes_a_quiet_skip() {
    let inspector = MutableInspector::new(TargetLifecycle::Frozen, TargetLiveness::Alive);
    let broker = SequenceBroker::new([]);
    let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);
    let start = Instant::now();

    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Deferred
    );
    inspector.set_lifecycle(TargetLifecycle::Exiting);
    assert_eq!(
        orchestrator
            .poll_deferred(start + Duration::from_secs(2))
            .unwrap(),
        Some(ProcessOutcome::Skipped)
    );
    assert!(orchestrator.deferred_targets().is_empty());
    assert_eq!(
        orchestrator.last_result(42, 142).unwrap().code,
        "target-exiting"
    );
    assert_eq!(broker.request_count(), 0);
}

#[test]
fn pre_resume_launch_failure_for_a_vanished_target_is_a_quiet_skip() {
    let inspector = MutableInspector::new(TargetLifecycle::Running, TargetLiveness::Vanished);
    let broker = SequenceBroker::new([broker_result(
        BrokerDisposition::LaunchFailed,
        "helper-launch-failed-before-resume",
    )]);
    let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);

    assert_eq!(
        orchestrator.handle_pid(42).unwrap(),
        ProcessOutcome::Skipped
    );
    assert_eq!(
        orchestrator.last_result(42, 142).unwrap().code,
        TARGET_VANISHED_RESULT_CODE
    );
    assert!(orchestrator.deferred_targets().is_empty());
}

#[test]
fn helper_launch_deferrals_double_to_the_cap_then_reject() {
    let inspector = MutableInspector::new(TargetLifecycle::Running, TargetLiveness::Alive);
    let launch_failure = || {
        broker_result(
            BrokerDisposition::LaunchFailed,
            "helper-launch-failed-before-resume",
        )
    };
    let broker = SequenceBroker::new((0..6).map(|_| launch_failure()));
    let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);
    let start = Instant::now();

    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Deferred
    );
    let expected_due = [2, 6, 14, 30, 62];
    for (index, seconds) in expected_due.into_iter().enumerate() {
        let target = orchestrator.deferred_targets().into_iter().next().unwrap();
        assert_eq!(target.deferrals, index as u8 + 1);
        assert_eq!(target.not_before, start + Duration::from_secs(seconds));
        assert_eq!(
            orchestrator.poll_deferred(target.not_before).unwrap(),
            Some(if index == 4 {
                ProcessOutcome::Rejected
            } else {
                ProcessOutcome::Deferred
            })
        );
    }
    assert_eq!(broker.request_count(), 6);
    assert!(orchestrator.deferred_targets().is_empty());
    let result = orchestrator.last_result(42, 142).unwrap();
    assert_eq!(result.outcome, ProcessOutcome::Rejected);
    assert_eq!(result.broker_disposition, BrokerDisposition::LaunchFailed);
    assert_eq!(result.code, "helper-launch-failed-before-resume");
    assert!(orchestrator.generation_health_error().is_none());
}

#[test]
fn broker_frozen_race_enters_the_frozen_deferral_queue() {
    let inspector = MutableInspector::new(TargetLifecycle::Running, TargetLiveness::Alive);
    let broker = SequenceBroker::new([broker_result(
        BrokerDisposition::TargetFrozen,
        "process-frozen",
    )]);
    let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);
    let start = Instant::now();

    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Deferred
    );
    let deferred = orchestrator.deferred_targets().into_iter().next().unwrap();
    assert_eq!(deferred.reason, DeferralReason::Frozen);
    assert_eq!(deferred.deferrals, 0);
    assert_eq!(deferred.not_before, start + Duration::from_secs(2));
}

#[test]
fn deferred_capacity_evicts_and_records_the_oldest_target() {
    let inspector = MutableInspector::new(TargetLifecycle::Frozen, TargetLiveness::Alive);
    let broker = SequenceBroker::new([]);
    let mut orchestrator = ProcessOrchestrator::new(u32::MAX, binding(), &inspector, &broker);
    let start = Instant::now();

    for pid in 1..=(MAX_DEFERRED_TARGETS as u32 + 1) {
        assert_eq!(
            orchestrator.handle_pid_at(pid, start).unwrap(),
            ProcessOutcome::Deferred
        );
    }
    assert_eq!(orchestrator.deferred_targets().len(), MAX_DEFERRED_TARGETS);
    assert_eq!(
        orchestrator.last_result(1, 101).unwrap().code,
        "deferral-capacity-exhausted"
    );
    assert_eq!(orchestrator.deferred_targets()[0].identity.pid, 2);
}

#[test]
fn session_changes_retain_other_sessions_and_overflow_clears_all_deferrals() {
    let inspector = MutableInspector::new(TargetLifecycle::Frozen, TargetLiveness::Alive);
    let broker = SequenceBroker::new([]);
    let mut orchestrator = ProcessOrchestrator::new(900, binding(), &inspector, &broker);
    let start = Instant::now();

    orchestrator.handle_pid_at(42, start).unwrap();
    orchestrator.handle_session_change(SessionChange {
        event_type: 6,
        session_id: 3,
    });
    assert_eq!(orchestrator.deferred_targets().len(), 1);
    orchestrator.handle_session_change(SessionChange {
        event_type: 6,
        session_id: 2,
    });
    assert!(orchestrator.deferred_targets().is_empty());

    orchestrator.handle_pid_at(43, start).unwrap();
    orchestrator.handle_session_change(SessionChange::overflow());
    assert!(orchestrator.deferred_targets().is_empty());
}
