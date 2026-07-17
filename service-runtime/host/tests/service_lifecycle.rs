use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use mactype_service_contract::{
    ComponentReadiness, HealthReport, HealthState, ReadinessReport, StructuredServiceError,
};
use mactype_service_host::{
    CompositeHealthPublisher, FileHealthPublisher, HealthPublisher, InitializedRuntime,
    RuntimeDriver, RuntimeHealthReporter, RuntimeInitializer, ScmState, ServiceRuntime,
    ServiceStatus, StatusReporter, StopSignal,
};

const PROFILE: &str = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

#[derive(Default)]
struct Recorder {
    events: Mutex<Vec<String>>,
}

impl StatusReporter for Recorder {
    fn report(&self, status: ServiceStatus) -> io::Result<()> {
        self.events
            .lock()
            .unwrap()
            .push(format!("scm:{:?}", status.state));
        Ok(())
    }
}

impl HealthPublisher for Recorder {
    fn publish(&self, report: &HealthReport) -> io::Result<()> {
        self.events
            .lock()
            .unwrap()
            .push(format!("health:{:?}", report.health));
        Ok(())
    }
}

struct ReadyInitializer;

impl RuntimeInitializer for ReadyInitializer {
    fn initialize(&self) -> Result<InitializedRuntime, StructuredServiceError> {
        Ok(InitializedRuntime::ready(
            Some(PROFILE.to_owned()),
            ReadinessReport::ready(),
        ))
    }
}

struct ImmediateStop;

impl StopSignal for ImmediateStop {
    fn wait(&self) -> Result<(), StructuredServiceError> {
        Ok(())
    }
}

#[test]
fn scm_running_and_internal_ready_are_reported_as_separate_events() {
    let recorder = Recorder::default();
    ServiceRuntime::new("0.2.0")
        .run(&recorder, &recorder, &ReadyInitializer, &ImmediateStop)
        .unwrap();

    assert_eq!(
        *recorder.events.lock().unwrap(),
        [
            "scm:StartPending",
            "health:Initializing",
            "scm:Running",
            "health:Ready",
            "scm:Stopped",
        ]
    );
}

#[test]
fn initialization_failure_never_reports_running_or_ready() {
    struct FailedInitializer;
    impl RuntimeInitializer for FailedInitializer {
        fn initialize(&self) -> Result<InitializedRuntime, StructuredServiceError> {
            Err(StructuredServiceError {
                code: "profile-invalid".to_owned(),
                message: "profile could not be opened".to_owned(),
                win32_error: Some(13),
            })
        }
    }

    let recorder = Recorder::default();
    assert!(ServiceRuntime::new("0.2.0")
        .run(&recorder, &recorder, &FailedInitializer, &ImmediateStop)
        .is_err());
    let events = recorder.events.lock().unwrap();
    assert!(events.contains(&"health:Failed".to_owned()));
    assert!(events.contains(&"scm:Stopped".to_owned()));
    assert!(!events.contains(&"scm:Running".to_owned()));
    assert!(!events.contains(&"health:Ready".to_owned()));
}

#[derive(Clone, Copy)]
enum LifecycleFault {
    StartPendingStatus,
    InitializingHealth,
    RunningStatus,
    FinalStoppedStatus,
}

struct FaultRecorder {
    fault: LifecycleFault,
    failed_once: AtomicBool,
    events: Mutex<Vec<String>>,
}

impl FaultRecorder {
    fn new(fault: LifecycleFault) -> Self {
        Self {
            fault,
            failed_once: AtomicBool::new(false),
            events: Mutex::new(Vec::new()),
        }
    }

    fn fail_once(&self, matches: bool) -> io::Result<()> {
        if matches && !self.failed_once.swap(true, Ordering::AcqRel) {
            return Err(io::Error::other("injected lifecycle reporter fault"));
        }
        Ok(())
    }
}

impl StatusReporter for FaultRecorder {
    fn report(&self, status: ServiceStatus) -> io::Result<()> {
        self.events
            .lock()
            .unwrap()
            .push(format!("scm:{:?}:{}", status.state, status.win32_exit_code));
        self.fail_once(match self.fault {
            LifecycleFault::StartPendingStatus => status.state == ScmState::StartPending,
            LifecycleFault::RunningStatus => status.state == ScmState::Running,
            LifecycleFault::FinalStoppedStatus => {
                status.state == ScmState::Stopped && status.win32_exit_code == 0
            }
            LifecycleFault::InitializingHealth => false,
        })
    }
}

impl HealthPublisher for FaultRecorder {
    fn publish(&self, report: &HealthReport) -> io::Result<()> {
        self.events
            .lock()
            .unwrap()
            .push(format!("health:{:?}", report.health));
        self.fail_once(
            matches!(self.fault, LifecycleFault::InitializingHealth)
                && report.health == HealthState::Initializing,
        )
    }
}

#[test]
fn every_lifecycle_reporter_fault_attempts_failed_health_and_stopped_with_error() {
    for fault in [
        LifecycleFault::StartPendingStatus,
        LifecycleFault::InitializingHealth,
        LifecycleFault::RunningStatus,
        LifecycleFault::FinalStoppedStatus,
    ] {
        let recorder = FaultRecorder::new(fault);

        assert!(ServiceRuntime::new("0.2.0")
            .run(&recorder, &recorder, &ReadyInitializer, &ImmediateStop)
            .is_err());

        let events = recorder.events.lock().unwrap();
        assert!(events.contains(&"health:Failed".to_owned()), "{events:?}");
        assert!(
            events.contains(&"scm:Stopped:1066".to_owned()),
            "{events:?}"
        );
    }
}

#[test]
fn incomplete_required_readiness_is_reported_as_failed_before_exit() {
    struct IncompleteInitializer;
    impl RuntimeInitializer for IncompleteInitializer {
        fn initialize(&self) -> Result<InitializedRuntime, StructuredServiceError> {
            Ok(InitializedRuntime::ready(
                Some(PROFILE.to_owned()),
                ReadinessReport {
                    profile: ComponentReadiness::Ready,
                    observer: ComponentReadiness::Initializing,
                    injector32: ComponentReadiness::NotRequired,
                    injector64: ComponentReadiness::NotRequired,
                },
            ))
        }
    }

    let recorder = Recorder::default();
    assert!(ServiceRuntime::new("0.2.0")
        .run(&recorder, &recorder, &IncompleteInitializer, &ImmediateStop)
        .is_err());
    let events = recorder.events.lock().unwrap();
    assert!(events.contains(&"health:Failed".to_owned()));
    assert!(events.contains(&"scm:Stopped".to_owned()));
    assert!(!events.contains(&"health:Ready".to_owned()));
}

#[test]
fn file_health_adapter_writes_a_versioned_machine_readable_report() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("health.json");
    let publisher = FileHealthPublisher::new(path.clone());
    let report = HealthReport::ready("0.2.0", Some(PROFILE.to_owned()));

    publisher.publish(&report).unwrap();
    let decoded: HealthReport = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(decoded, report);
    assert_eq!(decoded.health, HealthState::Ready);
    assert_eq!(FileHealthPublisher::read(&path).unwrap(), report);

    std::fs::write(&path, vec![b'x'; 16 * 1024 + 1]).unwrap();
    assert!(FileHealthPublisher::read(&path).is_err());
}

#[test]
fn composite_health_publisher_updates_pipe_and_persisted_snapshot_together() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("health.json");
    let file = FileHealthPublisher::new(path.clone());
    let recorder = Recorder::default();
    let composite = CompositeHealthPublisher::new(&recorder, &file);
    let report = HealthReport::ready("0.2.0", Some(PROFILE.to_owned()));

    composite.publish(&report).unwrap();

    assert_eq!(FileHealthPublisher::read(&path).unwrap(), report);
    assert!(recorder
        .events
        .lock()
        .unwrap()
        .contains(&"health:Ready".to_owned()));
}

#[test]
fn persisted_health_cleans_only_bounded_owned_staging_residue() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("health.json");
    let stale = directory.path().join(".health.json.new-123-456-1");
    let unrelated = directory.path().join(".health.json.new-operator-note");
    std::fs::write(&stale, b"partial").unwrap();
    std::fs::write(&unrelated, b"preserve").unwrap();

    FileHealthPublisher::new(path)
        .publish(&HealthReport::ready("0.2.0", Some(PROFILE.to_owned())))
        .unwrap();

    assert!(!stale.exists());
    assert!(unrelated.exists());
}

#[test]
fn persisted_health_refuses_oversized_service_root_without_cleanup_or_replacement() {
    const OVERSIZED_SERVICE_ROOT_ENTRY_COUNT: usize = 4097;
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("health.json");
    let stale = directory.path().join(".health.json.new-123-456-1");
    std::fs::write(&path, b"preserve-health").unwrap();
    std::fs::write(&stale, b"preserve-staging").unwrap();
    for index in 0..(OVERSIZED_SERVICE_ROOT_ENTRY_COUNT - 2) {
        std::fs::write(directory.path().join(format!("unrelated-{index}")), []).unwrap();
    }

    let error = FileHealthPublisher::new(path.clone())
        .publish(&HealthReport::ready("0.2.0", Some(PROFILE.to_owned())))
        .expect_err("oversized service root must fail closed");

    assert_eq!(error.kind(), io::ErrorKind::InvalidData);
    assert!(error.to_string().contains("entry count"), "{error}");
    assert_eq!(std::fs::read(path).unwrap(), b"preserve-health");
    assert_eq!(std::fs::read(stale).unwrap(), b"preserve-staging");
    assert_eq!(
        std::fs::read_dir(directory.path()).unwrap().count(),
        OVERSIZED_SERVICE_ROOT_ENTRY_COUNT
    );
}

#[test]
fn stop_statuses_are_nonzero_checkpoint_only_while_pending() {
    let recorder = Recorder::default();
    ServiceRuntime::new("0.2.0")
        .run(&recorder, &recorder, &ReadyInitializer, &ImmediateStop)
        .unwrap();

    let start = ServiceStatus::start_pending(1, 10_000);
    let stop = ServiceStatus::stop_pending(2, 5_000);
    assert_eq!(start.state, ScmState::StartPending);
    assert_eq!(start.checkpoint, 1);
    assert_eq!(stop.checkpoint, 2);
    assert_eq!(ServiceStatus::running().checkpoint, 0);
    assert_eq!(ServiceStatus::stopped().checkpoint, 0);
}

struct RecordingDriver {
    ran: Arc<AtomicBool>,
}

impl RuntimeDriver for RecordingDriver {
    fn run(
        &mut self,
        stop: &dyn StopSignal,
        _health: &dyn RuntimeHealthReporter,
    ) -> Result<(), StructuredServiceError> {
        self.ran.store(true, Ordering::Release);
        stop.wait()
    }
}

#[test]
fn ready_runtime_drives_the_process_observer_until_stop() {
    struct DrivenInitializer {
        ran: Arc<AtomicBool>,
    }
    impl RuntimeInitializer for DrivenInitializer {
        fn initialize(&self) -> Result<InitializedRuntime, StructuredServiceError> {
            Ok(InitializedRuntime::driven(
                Some(PROFILE.to_owned()),
                ReadinessReport::ready(),
                Box::new(RecordingDriver {
                    ran: self.ran.clone(),
                }),
            ))
        }
    }

    let ran = Arc::new(AtomicBool::new(false));
    let recorder = Recorder::default();
    ServiceRuntime::new("0.2.0")
        .run(
            &recorder,
            &recorder,
            &DrivenInitializer { ran: ran.clone() },
            &ImmediateStop,
        )
        .unwrap();

    assert!(ran.load(Ordering::Acquire));
}
