use super::*;
use crate::service_contract::{
    HealthState, InstallationState, RuntimeState, ServiceBackend, SystemServiceStatus,
};
use std::collections::VecDeque;

#[derive(Default)]
struct FakeMachineBackend {
    calls: Vec<&'static str>,
    status: Option<SystemServiceStatus>,
    executed: Option<(MachineAction, Vec<u8>)>,
    appinit_conflict: bool,
    appinit_error: Option<String>,
}

impl MachineBackend for FakeMachineBackend {
    fn new_service_status(&mut self) -> SystemServiceStatus {
        self.calls.push("status");
        self.status.clone().expect("test status")
    }

    fn execute(&mut self, _action: MachineAction, _profile: Option<&[u8]>) -> Result<(), String> {
        self.calls.push("execute");
        self.executed = Some((_action, _profile.unwrap_or_default().to_vec()));
        Ok(())
    }

    fn appinit_conflict(&mut self) -> Result<bool, String> {
        self.calls.push("appinit");
        match self.appinit_error.take() {
            Some(error) => Err(error),
            None => Ok(self.appinit_conflict),
        }
    }
}

fn ready_auto_service() -> SystemServiceStatus {
    SystemServiceStatus {
        backend: ServiceBackend::OpenSource,
        installation: InstallationState::Current,
        runtime: RuntimeState::Running,
        health: HealthState::Ready,
        binary_path: Some("fixed-service.exe".to_owned()),
        win32_error: None,
        active_profile_digest: Some("sha256:active".to_owned()),
        can_install: false,
        can_remove: true,
        can_start: false,
        can_stop: true,
        can_repair: true,
        can_upgrade: false,
    }
}

#[test]
fn login_tray_observes_only_and_never_mutates_the_machine() {
    let mut backend = FakeMachineBackend {
        status: Some(ready_auto_service()),
        ..Default::default()
    };

    assert_eq!(
        tray_login_with(&mut backend, false, false, Some("sha256:active")),
        TrayLoginState::UsingRunningNewService
    );
    assert_eq!(backend.calls, ["status"]);

    backend.calls.clear();
    assert_eq!(
        tray_login_with(&mut backend, true, false, Some("sha256:active")),
        TrayLoginState::Paused
    );
    assert_eq!(backend.calls, ["status"]);
}

#[test]
fn explicit_tray_apply_is_the_only_tray_path_that_publishes_profile_bytes() {
    let profile = b"[General]\r\nGammaValue=1.3\r\n";
    let mut backend = FakeMachineBackend {
        status: Some(ready_auto_service()),
        ..Default::default()
    };

    tray_apply_with(&mut backend, false, profile).unwrap();

    assert_eq!(backend.calls, ["appinit", "status", "execute"]);
    assert_eq!(
        backend.executed,
        Some((MachineAction::PublishProfile, profile.to_vec()))
    );
    assert!(tray_apply_with(&mut backend, true, profile)
        .unwrap_err()
        .contains("paused"));
}

#[test]
fn unsafe_machine_state_is_rejected_before_dispatch_without_retry() {
    let profile = b"[General]\r\nGammaValue=1.3\r\n";
    let mut foreign = FakeMachineBackend {
        status: Some(SystemServiceStatus {
            backend: ServiceBackend::Foreign,
            installation: InstallationState::Invalid,
            runtime: RuntimeState::Unknown,
            health: HealthState::Unknown,
            binary_path: Some("foreign.exe".to_owned()),
            win32_error: None,
            active_profile_digest: None,
            can_install: false,
            can_remove: false,
            can_start: false,
            can_stop: false,
            can_repair: false,
            can_upgrade: false,
        }),
        ..Default::default()
    };
    let error =
        execute_machine_action_with(&mut foreign, MachineAction::PublishProfile, Some(profile))
            .unwrap_err();
    assert!(
        error.contains("foreign") || error.contains("unsafe"),
        "{error}"
    );
    assert_eq!(foreign.calls, ["appinit", "status"]);

    let mut appinit = FakeMachineBackend {
        status: Some(ready_auto_service()),
        appinit_conflict: true,
        ..Default::default()
    };
    execute_machine_action_with(&mut appinit, MachineAction::Stop, None).unwrap();
    assert_eq!(appinit.calls, ["status", "execute"]);

    foreign.calls.clear();
    execute_machine_action_with(&mut foreign, MachineAction::Rollback, None).unwrap();
    assert_eq!(foreign.calls, ["status", "execute"]);
}

#[test]
fn native_actions_require_the_matching_backend_capability_before_dispatch() {
    for action in [
        MachineAction::Install,
        MachineAction::Upgrade,
        MachineAction::Repair,
        MachineAction::Remove,
        MachineAction::Start,
        MachineAction::Stop,
    ] {
        let mut status = ready_auto_service();
        status.can_install = false;
        status.can_upgrade = false;
        status.can_repair = false;
        status.can_remove = false;
        status.can_start = false;
        status.can_stop = false;

        let mut denied = FakeMachineBackend {
            status: Some(status.clone()),
            appinit_conflict: action == MachineAction::Stop,
            ..Default::default()
        };
        let error = execute_machine_action_with(&mut denied, action, None).unwrap_err();
        assert!(error.contains("authorize"), "{action:?}: {error}");
        assert!(denied.executed.is_none(), "{action:?} reached the broker");

        match action {
            MachineAction::Install => status.can_install = true,
            MachineAction::Upgrade => status.can_upgrade = true,
            MachineAction::Repair => status.can_repair = true,
            MachineAction::Remove => status.can_remove = true,
            MachineAction::Start => status.can_start = true,
            MachineAction::Stop => status.can_stop = true,
            _ => unreachable!(),
        }
        let mut allowed = FakeMachineBackend {
            status: Some(status),
            appinit_conflict: action == MachineAction::Stop,
            ..Default::default()
        };
        execute_machine_action_with(&mut allowed, action, None).unwrap();
        assert_eq!(allowed.executed, Some((action, Vec::new())));
    }
}

#[test]
fn appinit_conflict_never_turns_an_unrelated_native_capability_into_stop() {
    let mut status = ready_auto_service();
    status.can_stop = false;
    status.can_start = true;
    let mut backend = FakeMachineBackend {
        status: Some(status),
        appinit_conflict: true,
        ..Default::default()
    };

    let error = execute_machine_action_with(&mut backend, MachineAction::Start, None).unwrap_err();

    assert!(error.contains("AppInit"), "{error}");
    assert!(backend.executed.is_none());
}

#[test]
fn verified_stop_does_not_depend_on_reading_appinit_registry_state() {
    let mut stop = FakeMachineBackend {
        status: Some(ready_auto_service()),
        appinit_error: Some("registry unavailable".to_owned()),
        ..Default::default()
    };

    execute_machine_action_with(&mut stop, MachineAction::Stop, None).unwrap();

    assert_eq!(stop.calls, ["status", "execute"]);

    let mut start_status = ready_auto_service();
    start_status.can_start = true;
    let mut start = FakeMachineBackend {
        status: Some(start_status),
        appinit_error: Some("registry unavailable".to_owned()),
        ..Default::default()
    };

    let error = execute_machine_action_with(&mut start, MachineAction::Start, None).unwrap_err();

    assert!(error.contains("registry unavailable"), "{error}");
    assert_eq!(start.calls, ["appinit"]);
    assert!(start.executed.is_none());
}

#[test]
fn appinit_status_projects_only_the_verified_stop_capability() {
    let projected = project_new_service_capabilities(ready_auto_service(), true);

    assert!(!projected.can_install);
    assert!(!projected.can_remove);
    assert!(!projected.can_start);
    assert!(projected.can_stop);
    assert!(!projected.can_repair);
    assert!(!projected.can_upgrade);

    let mut foreign = ready_auto_service();
    foreign.backend = ServiceBackend::Foreign;
    let projected = project_new_service_capabilities(foreign, true);
    assert!(!projected.can_stop);

    let mut not_authorized = ready_auto_service();
    not_authorized.can_stop = false;
    let projected = project_new_service_capabilities(not_authorized, true);
    assert!(!projected.can_stop);
}

#[test]
fn registry_conflict_never_claims_verified_system_injection() {
    let service = ready_auto_service();

    assert!(project_system_injection_active(
        &service,
        false,
        Some("sha256:active")
    ));
    assert!(!project_system_injection_active(
        &service,
        true,
        Some("sha256:active")
    ));
    assert!(project_new_service_capabilities(service, true).can_stop);
}

#[test]
fn appinit_blocks_only_enabled_mactype_dlls_and_fails_closed() {
    let mactype = "C:\\Program Files\\MacType\\MacType64.dll\0"
        .encode_utf16()
        .collect::<Vec<_>>();
    let other = "C:\\Other\\OtherHook.dll\0"
        .encode_utf16()
        .collect::<Vec<_>>();
    assert!(appinit_view_conflict(Ok(true), Ok(Some(mactype))).unwrap());
    assert!(!appinit_view_conflict(Ok(false), Err(())).unwrap());
    assert!(!appinit_view_conflict(Ok(true), Ok(Some(other))).unwrap());
    assert!(appinit_view_conflict(Err(()), Ok(None)).is_err());
    assert!(appinit_view_conflict(Ok(true), Ok(Some(vec![b'M' as u16]))).is_err());
}

#[test]
fn publish_profile_orders_running_stopped_and_absent_service_activation() {
    struct PublishBackend {
        states: VecDeque<SystemServiceStatus>,
        actions: Vec<MachineAction>,
    }

    impl MachineBackend for PublishBackend {
        fn new_service_status(&mut self) -> SystemServiceStatus {
            self.states.pop_front().expect("test service state")
        }

        fn appinit_conflict(&mut self) -> Result<bool, String> {
            Ok(false)
        }

        fn execute(
            &mut self,
            action: MachineAction,
            _profile: Option<&[u8]>,
        ) -> Result<(), String> {
            self.actions.push(action);
            Ok(())
        }
    }

    let profile = b"[General]\r\nGammaValue=1.3\r\n";
    let digest = mactype_service_contract::GenerationId::from_profile_bytes(profile)
        .as_str()
        .to_owned();
    let ready = |installation| SystemServiceStatus {
        installation,
        active_profile_digest: Some(digest.clone()),
        ..ready_auto_service()
    };
    let before = |installation, runtime| SystemServiceStatus {
        installation,
        runtime,
        health: HealthState::Unknown,
        active_profile_digest: None,
        ..ready_auto_service()
    };

    for (initial, expected) in [
        (
            before(InstallationState::Current, RuntimeState::Running),
            vec![
                MachineAction::Stop,
                MachineAction::PublishProfile,
                MachineAction::Start,
            ],
        ),
        (
            before(InstallationState::Current, RuntimeState::Stopped),
            vec![MachineAction::PublishProfile, MachineAction::Start],
        ),
        (
            before(InstallationState::Absent, RuntimeState::Stopped),
            vec![
                MachineAction::PublishProfile,
                MachineAction::Install,
                MachineAction::Start,
            ],
        ),
        (
            before(InstallationState::Outdated, RuntimeState::Stopped),
            vec![
                MachineAction::PublishProfile,
                MachineAction::Upgrade,
                MachineAction::Start,
            ],
        ),
    ] {
        let mut backend = PublishBackend {
            states: VecDeque::from([initial, ready(InstallationState::Current)]),
            actions: Vec::new(),
        };
        publish_profile_transaction_with(&mut backend, profile).unwrap();
        assert_eq!(backend.actions, expected);
    }
}

struct FailingPublishBackend {
    states: VecDeque<SystemServiceStatus>,
    results: VecDeque<Result<(), String>>,
    actions: Vec<MachineAction>,
}

impl MachineBackend for FailingPublishBackend {
    fn new_service_status(&mut self) -> SystemServiceStatus {
        self.states.pop_front().expect("test service state")
    }

    fn appinit_conflict(&mut self) -> Result<bool, String> {
        Ok(false)
    }

    fn execute(&mut self, action: MachineAction, _profile: Option<&[u8]>) -> Result<(), String> {
        self.actions.push(action);
        self.results.pop_front().unwrap_or(Ok(()))
    }
}

#[test]
fn publish_failure_reports_a_failed_restart_as_cleanup_unknown() {
    let mut backend = FailingPublishBackend {
        states: VecDeque::from([ready_auto_service()]),
        results: VecDeque::from([
            Ok(()),
            Err("publish failed".to_owned()),
            Err("restart failed".to_owned()),
        ]),
        actions: Vec::new(),
    };

    let error = publish_profile_transaction_with(&mut backend, b"[General]\r\n").unwrap_err();

    assert!(error.contains("publish failed"), "{error}");
    assert!(error.contains("restart failed"), "{error}");
    assert!(error.contains("cleanup is unknown"), "{error}");
    assert_eq!(
        backend.actions,
        [
            MachineAction::Stop,
            MachineAction::PublishProfile,
            MachineAction::Start,
        ]
    );
}

#[test]
fn activation_failure_reports_every_failed_rollback_step() {
    let mut backend = FailingPublishBackend {
        states: VecDeque::from([ready_auto_service()]),
        results: VecDeque::from([
            Ok(()),
            Ok(()),
            Err("activation failed".to_owned()),
            Err("cleanup stop failed".to_owned()),
            Err("profile rollback failed".to_owned()),
            Err("prior restart failed".to_owned()),
        ]),
        actions: Vec::new(),
    };

    let error = publish_profile_transaction_with(&mut backend, b"[General]\r\n").unwrap_err();

    for expected in [
        "activation failed",
        "cleanup stop failed",
        "profile rollback failed",
        "prior restart failed",
        "cleanup is unknown",
    ] {
        assert!(error.contains(expected), "missing {expected}: {error}");
    }
    assert_eq!(
        backend.actions,
        [
            MachineAction::Stop,
            MachineAction::PublishProfile,
            MachineAction::Start,
            MachineAction::Stop,
            MachineAction::Rollback,
            MachineAction::Start,
        ]
    );
}
