// Hosted-CI live proof for the legacy MacTray teardown paths. These tests
// exercise the production guard against a real MacType installation staged
// from the official installer on a disposable GitHub-hosted runner. The
// module lives only on codex/hosted-teardown-proof and must not be merged
// onward; every test refuses to run without the workflow's environment gate
// because it mutates the running machine.

use super::legacy_mactray::{
    self, LegacyTrayConflictState, LegacyTrayExitRequest, LegacyTrayProcessState,
    LegacyTrayStartupCoordinator, LegacyTrayStartupState, LegacyTrayStatus, ServicePresence,
    ServiceRuntimeState,
};
use super::legacy_migration::{disable_startup_scope, restore_startup_scope, StartupReceiptScope};

fn require_proof_environment() {
    if std::env::var("MACTYPE_HOSTED_TEARDOWN_PROOF").as_deref() != Ok("1") {
        panic!(
            "this live proof mutates the running machine; it only runs inside the \
             hosted-teardown-proof workflow with MACTYPE_HOSTED_TEARDOWN_PROOF=1"
        );
    }
}

fn observed_status() -> LegacyTrayStatus {
    let status = legacy_mactray::tray_status();
    println!("observed tray status: {status:?}");
    status
}

#[test]
#[ignore = "live proof; only meaningful inside the hosted-teardown-proof workflow"]
fn guard_reports_clear_before_any_legacy_artifacts() {
    require_proof_environment();
    let status = observed_status();
    assert!(
        matches!(status.process, LegacyTrayProcessState::Absent),
        "no interactive MacTray may run before the proof stages one"
    );
    assert!(
        matches!(status.startup, LegacyTrayStartupState::Absent),
        "no MacTray autostart entries may exist before the proof stages them"
    );
    assert!(matches!(status.conflict, LegacyTrayConflictState::Clear));
}

#[test]
#[ignore = "live proof; only meaningful inside the hosted-teardown-proof workflow"]
fn guard_detects_real_tray_mode_conflict() {
    require_proof_environment();
    let status = observed_status();
    let LegacyTrayProcessState::TrustedCurrentSession {
        pid,
        creation_time,
        path,
    } = &status.process
    else {
        panic!("the real MacTray must classify as a trusted current-session tray process");
    };
    println!(
        "trusted tray process: pid={pid} creation_time={creation_time} path={}",
        path.display()
    );
    assert!(matches!(status.conflict, LegacyTrayConflictState::Detected));
    assert!(
        status.can_request_exit,
        "the guard must expose the graceful exit action for the running tray process"
    );
}

#[test]
#[ignore = "live proof; only meaningful inside the hosted-teardown-proof workflow"]
fn graceful_exit_stops_real_tray_mode() {
    require_proof_environment();
    let status = observed_status();
    let LegacyTrayProcessState::TrustedCurrentSession {
        pid,
        creation_time,
        path,
    } = status.process
    else {
        panic!("the real MacTray must be running for the graceful-exit proof");
    };
    let request = LegacyTrayExitRequest {
        pid,
        creation_time,
        path,
    };
    legacy_mactray::request_tray_exit(&request)
        .expect("the production graceful exit must stop the real tray-mode MacTray");
    let after = observed_status();
    assert!(
        matches!(after.process, LegacyTrayProcessState::Absent),
        "MacTray must be gone after the graceful exit"
    );
    println!("graceful exit verified: the real MacTray exited on the official protocol");
}

struct HostedProofStartupCoordinator;

impl LegacyTrayStartupCoordinator for HostedProofStartupCoordinator {
    fn observe_status(&mut self) -> LegacyTrayStatus {
        legacy_mactray::tray_status()
    }

    fn disable_current_user(&mut self) -> Result<(), String> {
        disable_startup_scope(StartupReceiptScope::CurrentUser)
    }

    // The hosted runner is already elevated, so the broker's local-machine
    // action runs in-process instead of bouncing through the UAC bridge.
    fn disable_local_machine(&mut self) -> Result<(), String> {
        disable_startup_scope(StartupReceiptScope::LocalMachine)
    }

    fn restore_local_machine(&mut self) -> Result<(), String> {
        restore_startup_scope(StartupReceiptScope::LocalMachine)
    }

    fn restore_current_user(&mut self) -> Result<(), String> {
        restore_startup_scope(StartupReceiptScope::CurrentUser)
    }
}

#[test]
#[ignore = "live proof; only meaningful inside the hosted-teardown-proof workflow"]
fn autostart_disable_removes_staged_entries() {
    require_proof_environment();
    let before = observed_status();
    assert!(
        matches!(before.process, LegacyTrayProcessState::Absent),
        "MacTray must be absent before its startup entries can be disabled"
    );
    let LegacyTrayStartupState::Detected { entries } = &before.startup else {
        panic!("the staged Run entries must classify as removable owned startup entries");
    };
    assert!(!entries.is_empty());
    for entry in entries {
        println!("staged startup entry: {entry:?}");
    }
    legacy_mactray::disable_legacy_tray_startup_with(&mut HostedProofStartupCoordinator)
        .expect("the production autostart disable must remove the staged entries");
    let after = observed_status();
    assert!(
        matches!(after.startup, LegacyTrayStartupState::Absent),
        "no startup entries may remain after the disable"
    );
    assert!(matches!(after.conflict, LegacyTrayConflictState::Clear));
    println!("autostart disable verified: both scopes are clear");
}

#[test]
#[ignore = "live proof; only meaningful inside the hosted-teardown-proof workflow"]
fn service_mode_is_exempt_from_tray_conflict() {
    require_proof_environment();
    let service = legacy_mactray::status(false);
    println!("observed legacy service status: {service:?}");
    assert!(
        matches!(service.presence, ServicePresence::Owned),
        "the staged service must classify as the owned legacy MacType service"
    );
    assert!(
        matches!(service.state, ServiceRuntimeState::Running),
        "the staged service must be running for the stop proof"
    );
    assert!(service.can_stop);
    let tray = observed_status();
    assert!(
        matches!(tray.process, LegacyTrayProcessState::Absent),
        "the session-zero service process must stay exempt from tray-mode classification"
    );
    assert!(matches!(tray.conflict, LegacyTrayConflictState::Clear));
}

#[test]
#[ignore = "live proof; only meaningful inside the hosted-teardown-proof workflow"]
fn service_migration_stop_remove_restore_roundtrip() {
    require_proof_environment();
    let before = legacy_mactray::status(false);
    println!("service before migration: {before:?}");
    assert!(matches!(before.presence, ServicePresence::Owned));
    let initially_running = matches!(before.state, ServiceRuntimeState::Running);
    let snapshot = legacy_mactray::migration_snapshot(false)
        .expect("the legacy SCM snapshot must be capturable");
    legacy_mactray::validate_migration_snapshot(&snapshot)
        .expect("the captured snapshot must validate for restore");
    legacy_mactray::stop_for_migration().expect("the legacy service must stop");
    let stopped = legacy_mactray::status(false);
    println!("service after stop: {stopped:?}");
    assert!(matches!(stopped.state, ServiceRuntimeState::Stopped));
    legacy_mactray::remove_for_migration().expect("the legacy service must be removable");
    let removed = legacy_mactray::status(false);
    println!("service after remove: {removed:?}");
    assert!(matches!(removed.presence, ServicePresence::Absent));
    legacy_mactray::restore_configuration_after_migration(&snapshot)
        .expect("the legacy service configuration must restore from the snapshot");
    legacy_mactray::restore_running_state_after_migration(&snapshot)
        .expect("the legacy service running state must restore from the snapshot");
    let restored = legacy_mactray::status(false);
    println!("service after restore: {restored:?}");
    assert!(matches!(restored.presence, ServicePresence::Owned));
    if initially_running {
        assert!(
            matches!(restored.state, ServiceRuntimeState::Running),
            "the service must run again after the roundtrip"
        );
    }
    println!("SCM migration roundtrip verified: stop -> remove -> restore");
}
