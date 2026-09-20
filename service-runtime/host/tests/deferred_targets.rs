#[path = "support/broker.rs"]
mod broker_support;
#[path = "support/event_sink.rs"]
mod event_sink;
#[path = "support/identity.rs"]
mod identity_support;
#[path = "support/inspector.rs"]
mod inspector_support;

use std::time::{Duration, Instant};

use broker_support::ScriptedBroker;
use event_sink::discard_events;
use identity_support::{binding, identity};
use inspector_support::{InspectorResponse, ScriptedInspector};
use mactype_service_contract::{ConsoleProcessPolicy, PrivateFreeTypePolicy, UnityFontHookPolicy};
use mactype_service_host::{
    BinarySignaturePolicy, BrokerDisposition, BrokerResult, DeferralReason, DynamicCodePolicy,
    ImageSubsystem, InspectionEvidence, ProcessIdentity, ProcessInspection, ProcessOrchestrator,
    ProcessOutcome, SessionChange, TargetLifecycle, TargetLiveness, MAX_DEFERRED_TARGETS,
    TARGET_VANISHED_RESULT_CODE,
};

fn target(pid: u32) -> ProcessIdentity {
    identity(pid, u64::from(pid) + 100)
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

fn response(
    identity: ProcessIdentity,
    lifecycle: TargetLifecycle,
    liveness: TargetLiveness,
) -> (u32, InspectorResponse) {
    (
        identity.pid,
        InspectorResponse::inspected(inspection(identity), lifecycle).with_liveness(liveness),
    )
}

fn console_response(
    identity: ProcessIdentity,
    subsystem: ImageSubsystem,
    age: Option<Duration>,
    liveness: TargetLiveness,
) -> (u32, InspectorResponse) {
    (
        identity.pid,
        InspectorResponse::inspected(inspection(identity), TargetLifecycle::Running)
            .with_liveness(liveness)
            .with_subsystem(subsystem)
            .with_process_age(age),
    )
}

fn broker_result(disposition: BrokerDisposition, code: &str) -> BrokerResult {
    BrokerResult::new(disposition, code, None)
}

#[test]
fn fresh_console_target_waits_only_for_the_remaining_grace_then_injects() {
    let target = target(42);
    let inspector = ScriptedInspector::new([
        console_response(
            target.clone(),
            ImageSubsystem::Console,
            Some(Duration::from_millis(750)),
            TargetLiveness::Alive,
        ),
        console_response(
            target.clone(),
            ImageSubsystem::Console,
            Some(Duration::from_millis(750)),
            TargetLiveness::Alive,
        ),
        console_response(
            target.clone(),
            ImageSubsystem::Console,
            Some(Duration::from_millis(750)),
            TargetLiveness::Alive,
        ),
    ]);
    let broker = ScriptedBroker::new([broker_result(
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
        discard_events(),
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
    assert_eq!(inspector.liveness_probes().len(), 1);
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
        let target = target(42);
        let inspector = ScriptedInspector::new([console_response(
            target,
            subsystem,
            age,
            TargetLiveness::Alive,
        )]);
        let broker = ScriptedBroker::new([broker_result(
            BrokerDisposition::Injected,
            "renderer-active",
        )]);
        let mut orchestrator =
            ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());

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
    let target = target(42);
    let inspector = ScriptedInspector::new([console_response(
        target,
        ImageSubsystem::Console,
        Some(Duration::ZERO),
        TargetLiveness::Vanished,
    )]);
    let broker = ScriptedBroker::new([]);
    let mut orchestrator =
        ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());
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
    let original = target(42);
    let reused = ProcessIdentity {
        creation_time: original.creation_time + 1,
        ..original.clone()
    };
    let inspector = ScriptedInspector::new([
        console_response(
            original,
            ImageSubsystem::Console,
            Some(Duration::ZERO),
            TargetLiveness::Alive,
        ),
        console_response(
            reused,
            ImageSubsystem::Console,
            Some(Duration::ZERO),
            TargetLiveness::Alive,
        ),
    ]);
    let broker = ScriptedBroker::new([]);
    let mut orchestrator =
        ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());
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
    let target = target(42);
    let inspector = ScriptedInspector::new([
        response(
            target.clone(),
            TargetLifecycle::Frozen,
            TargetLiveness::Alive,
        ),
        response(
            target.clone(),
            TargetLifecycle::Frozen,
            TargetLiveness::Alive,
        ),
        response(
            target.clone(),
            TargetLifecycle::Running,
            TargetLiveness::Alive,
        ),
    ]);
    let broker = ScriptedBroker::new([broker_result(
        BrokerDisposition::Injected,
        "renderer-active",
    )]);
    let mut orchestrator =
        ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());
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

    inspector.set_lifecycle(&target, TargetLifecycle::Running);
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
    let target = target(42);
    let inspector = ScriptedInspector::new([response(
        target.clone(),
        TargetLifecycle::Frozen,
        TargetLiveness::Alive,
    )]);
    let broker = ScriptedBroker::new([]);
    let mut orchestrator =
        ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());
    let start = Instant::now();

    assert_eq!(
        orchestrator.handle_pid_at(42, start).unwrap(),
        ProcessOutcome::Deferred
    );
    inspector.set_lifecycle(&target, TargetLifecycle::Exiting);
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
    let inspector = ScriptedInspector::new([response(
        target(42),
        TargetLifecycle::Running,
        TargetLiveness::Vanished,
    )]);
    let broker = ScriptedBroker::new([broker_result(
        BrokerDisposition::LaunchFailed,
        "helper-launch-failed-before-resume",
    )]);
    let mut orchestrator =
        ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());

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
    let inspector = ScriptedInspector::new(
        (0..7).map(|_| response(target(42), TargetLifecycle::Running, TargetLiveness::Alive)),
    );
    let launch_failure = || {
        broker_result(
            BrokerDisposition::LaunchFailed,
            "helper-launch-failed-before-resume",
        )
    };
    let broker = ScriptedBroker::new((0..6).map(|_| launch_failure()));
    let mut orchestrator =
        ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());
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
    let inspector = ScriptedInspector::new([response(
        target(42),
        TargetLifecycle::Running,
        TargetLiveness::Alive,
    )]);
    let broker = ScriptedBroker::new([broker_result(
        BrokerDisposition::TargetFrozen,
        "process-frozen",
    )]);
    let mut orchestrator =
        ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());
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
    let inspector = ScriptedInspector::new(
        (1..=(MAX_DEFERRED_TARGETS as u32 + 1))
            .map(|pid| response(target(pid), TargetLifecycle::Frozen, TargetLiveness::Alive)),
    );
    let broker = ScriptedBroker::new([]);
    let mut orchestrator =
        ProcessOrchestrator::new(u32::MAX, binding(), &inspector, &broker, discard_events());
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
    let inspector = ScriptedInspector::new([
        response(target(42), TargetLifecycle::Frozen, TargetLiveness::Alive),
        response(target(43), TargetLifecycle::Frozen, TargetLiveness::Alive),
    ]);
    let broker = ScriptedBroker::new([]);
    let mut orchestrator =
        ProcessOrchestrator::new(900, binding(), &inspector, &broker, discard_events());
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
