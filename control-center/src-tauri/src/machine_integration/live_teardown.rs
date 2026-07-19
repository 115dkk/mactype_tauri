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
use super::legacy_migration::{
    disable_startup_scope, prepare_backup, remove_after_verified, restore_startup_scope, rollback,
    stop_legacy, RemovalVerification, StartupReceiptScope,
};
use super::{execute_machine_action_with, MachineAction, MachineBackend};
use crate::service_contract::{
    HealthState, InstallationState, RuntimeState, ServiceBackend, SystemServiceStatus,
};

// SERVICE_DISABLED: a service with this start type never auto-starts at boot.
const SERVICE_DISABLED_START_TYPE: u32 = 4;

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
    // R2: the stop must also disable the start type so a reboot between the
    // migration and the funeral cannot auto-start the legacy service.
    let after_stop = legacy_mactray::migration_snapshot(false)
        .expect("a stopped legacy service must still be snapshot-capturable");
    assert_eq!(
        after_stop.configuration.start_type, SERVICE_DISABLED_START_TYPE,
        "migration stop must leave the legacy service start type DISABLED (R2)"
    );
    println!("R2 verified: legacy service start type is DISABLED after migration stop");
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
    let restored_snapshot = legacy_mactray::migration_snapshot(false)
        .expect("the restored legacy service must be snapshot-capturable");
    assert_eq!(
        restored_snapshot.configuration.start_type, snapshot.configuration.start_type,
        "restore must put the original legacy service start type back (undo the R2 disable)"
    );
    if initially_running {
        assert!(
            matches!(restored.state, ServiceRuntimeState::Running),
            "the service must run again after the roundtrip"
        );
    }
    println!("SCM migration roundtrip verified: stop -> remove -> restore (start type restored)");
}

#[test]
#[ignore = "live proof; only meaningful inside the hosted-teardown-proof workflow"]
fn legacy_service_funeral_through_backup_receipt_transaction() {
    require_proof_environment();

    // The legacy service is itself a retirement target. This exercises the
    // product's real retirement transaction — the same backup receipt, stop,
    // verified removal, and rollback the control center drives — against the
    // staged real-shape MacType service. The RemovalVerification is the
    // caller's certification that the replacement is healthy; the upstream
    // computation of that certification is covered by the open-service CI, so
    // here we prove the transaction honours the gate and is fully reversible.
    let before = legacy_mactray::status(false);
    println!("legacy service before funeral: {before:?}");
    assert!(
        matches!(before.presence, ServicePresence::Owned),
        "the staged legacy service must be owned before its funeral"
    );
    assert!(
        matches!(before.state, ServiceRuntimeState::Running),
        "the staged legacy service must be running before its funeral"
    );

    // 1. Back up the legacy service (SCM snapshot + profiles + registry export).
    prepare_backup().expect("the legacy migration backup must be preparable");

    // 2. Stop it as the first irreversible-looking step of the transaction.
    stop_legacy().expect("the legacy service must stop under the transaction");
    let stopped = legacy_mactray::status(false);
    println!("legacy service after transaction stop: {stopped:?}");
    assert!(matches!(stopped.presence, ServicePresence::Owned));
    assert!(matches!(stopped.state, ServiceRuntimeState::Stopped));

    // 3. The removal gate must REFUSE an incomplete verification, and the
    //    service must survive the refusal untouched.
    let incomplete = RemovalVerification {
        new_service_ready: false,
        active_digest_match: true,
        backup_valid: true,
    };
    let refused = remove_after_verified(incomplete);
    println!("removal refused without a ready replacement: {refused:?}");
    assert!(
        refused.is_err(),
        "verified removal must refuse an incomplete verification"
    );
    assert!(
        matches!(
            legacy_mactray::status(false).presence,
            ServicePresence::Owned
        ),
        "a refused removal must leave the legacy service intact"
    );

    // 4. With the full caller certification, the removal executes.
    let authorized = RemovalVerification {
        new_service_ready: true,
        active_digest_match: true,
        backup_valid: true,
    };
    remove_after_verified(authorized).expect("verified legacy removal must succeed");
    let removed = legacy_mactray::status(false);
    println!("legacy service after verified removal: {removed:?}");
    assert!(
        matches!(removed.presence, ServicePresence::Absent),
        "the legacy service must be gone after verified removal"
    );

    // 5. Safety net: the receipt-based rollback resurrects the exact service.
    rollback().expect("the receipt-based rollback must restore the legacy service");
    let restored = legacy_mactray::status(false);
    println!("legacy service after rollback: {restored:?}");
    assert!(
        matches!(restored.presence, ServicePresence::Owned),
        "rollback must restore the legacy service"
    );
    assert!(
        matches!(restored.state, ServiceRuntimeState::Running),
        "rollback must return the legacy service to running"
    );
    println!(
        "legacy service funeral verified: backup -> stop -> gated removal -> rollback restores it"
    );
}

// A machine backend that reports a stopped, installable new service but delegates
// the legacy-service observation to the real production primitive, so the
// orchestrator gate is exercised against the actual staged legacy service. Its
// execute() must never run: a refusal has to happen before any mutation.
struct RealLegacyProbeBackend;

impl MachineBackend for RealLegacyProbeBackend {
    fn new_service_status(&mut self) -> SystemServiceStatus {
        SystemServiceStatus {
            backend: ServiceBackend::OpenSource,
            installation: InstallationState::Absent,
            runtime: RuntimeState::Stopped,
            health: HealthState::Unknown,
            binary_path: None,
            win32_error: None,
            active_profile_digest: None,
            can_install: true,
            can_remove: false,
            can_start: true,
            can_stop: false,
            can_repair: false,
            can_upgrade: false,
        }
    }

    fn legacy_tray_status(&mut self) -> LegacyTrayStatus {
        LegacyTrayStatus::clear()
    }

    fn appinit_conflict(&mut self) -> Result<bool, String> {
        Ok(false)
    }

    fn legacy_service_present(&mut self) -> Result<bool, String> {
        legacy_mactray::legacy_service_present()
    }

    fn execute(&mut self, action: MachineAction, _profile: Option<&[u8]>) -> Result<(), String> {
        panic!(
            "the new service must never be mutated while a legacy service is present: {action:?}"
        );
    }
}

#[test]
#[ignore = "live proof; only meaningful inside the hosted-teardown-proof workflow"]
fn generic_activation_refuses_against_a_live_legacy_service() {
    require_proof_environment();

    // R1: while a real legacy MacType service is installed, install / start /
    // apply-profile must all refuse before any mutation, so the new injector is
    // never started alongside the legacy one. Retirement must go through Migrate.
    let service = legacy_mactray::status(false);
    println!("legacy service before the activation-refusal proof: {service:?}");
    assert!(
        matches!(service.presence, ServicePresence::Owned),
        "the real legacy service must be present for the R1 activation-refusal proof"
    );
    assert!(
        legacy_mactray::legacy_service_present().expect("legacy presence must be observable"),
        "the guard's presence primitive must observe the real legacy service"
    );

    let mut backend = RealLegacyProbeBackend;
    for action in [MachineAction::Install, MachineAction::Start] {
        let error = execute_machine_action_with(&mut backend, action, None)
            .expect_err("activation must refuse while the legacy service is present");
        println!("{action:?} refused: {error}");
        assert!(
            error.contains("legacy MacType service"),
            "{action:?}: {error}"
        );
    }
    let profile = b"[General]\r\nGammaValue=1.3\r\n";
    let error =
        execute_machine_action_with(&mut backend, MachineAction::PublishProfile, Some(profile))
            .expect_err("apply profile must refuse while the legacy service is present");
    println!("PublishProfile refused: {error}");
    assert!(error.contains("legacy MacType service"), "{error}");
    println!(
        "R1 verified: install/start/apply all refuse while the real legacy service is present"
    );
}
