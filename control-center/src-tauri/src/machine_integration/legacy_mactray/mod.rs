mod model;
mod ownership;
mod restore_contract;
mod startup_coordinator;
mod tray_exit;
mod tray_process;
mod tray_startup;

#[cfg(test)]
use model::ServiceTriggerConfiguration;
use model::{require_stable_migration_state, snapshot_trigger_configuration};
pub(crate) use model::{
    FailureAction, FailureActionsConfiguration, LegacyScmSnapshot, SecurityDescriptorSnapshot,
    ServiceConfiguration, ServiceExtendedConfiguration,
};
pub use model::{LegacyServiceStatus, ServicePresence, ServiceRuntimeState};
pub(crate) use model::{
    LegacyTrayConflictState, LegacyTrayProcessState, LegacyTrayStartupEntry,
    LegacyTrayStartupSource, LegacyTrayStartupState, LegacyTrayStatus,
};
#[cfg(test)]
use ownership::{classify_configuration, owned_service_configuration};
use ownership::{is_trusted_mactray_layout, status_from_configuration, with_capabilities};
#[cfg(test)]
use restore_contract::SERVICE_RESTORE_ORDER;
use restore_contract::{
    perform_service_configuration_restore, verify_restored_configuration,
    ServiceConfigurationRestorer, ServiceRestoreStep,
};
#[cfg(test)]
use std::path::Path;
use std::path::PathBuf;
#[cfg(test)]
use tray_exit::{request_tray_exit_with, LegacyTrayExitBackend, LegacyTrayExitOutcome};
pub(crate) use tray_process::observe_tray_process;
#[cfg(test)]
use tray_process::{
    classify_tray_process_inventory, LegacyTrayProcessIdentity, LegacyTrayProcessObservation,
};

pub(crate) fn tray_status() -> LegacyTrayStatus {
    let mut status =
        LegacyTrayStatus::from_states(observe_tray_process(), tray_startup::observe_tray_startup());
    status.can_request_exit &= tray_exit::official_exit_available(&status.process);
    status
}

pub(crate) use startup_coordinator::{
    disable_legacy_tray_startup_with, LegacyTrayStartupCoordinator,
};
pub(crate) use tray_exit::{request_tray_exit, LegacyTrayExitRequest};
#[cfg(test)]
use tray_startup::{
    classify_startup_command, classify_startup_inventory, disable_startup_with,
    is_legacy_tray_startup_candidate, startup_source_requires_current_user_sid,
    LegacyTrayStartupObservation, StartupDisableBackend, StartupMutationEvent,
    StartupTargetClassification,
};
pub(crate) use tray_startup::{
    observe_owned_tray_startup, plan_startup_restore, read_tray_startup_artifact_bytes,
    remove_tray_startup_artifact_exact, restore_tray_startup_artifact_if_absent, same_windows_path,
    LegacyTrayStartupArtifact, LegacyTrayStartupLocator, LegacyTrayStartupScope,
    StartupRestoreAction,
};

#[cfg(windows)]
mod windows;

pub(crate) fn trusted_installation_root() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        windows::expected_mactray_path().and_then(|path| path.parent().map(PathBuf::from))
    }
    #[cfg(not(windows))]
    {
        None
    }
}

pub(crate) fn migration_snapshot(registry_conflict: bool) -> Result<LegacyScmSnapshot, String> {
    #[cfg(windows)]
    {
        windows::migration_snapshot(registry_conflict)
    }
    #[cfg(not(windows))]
    {
        let _ = registry_conflict;
        Err("legacy SCM migration is available only on Windows".to_owned())
    }
}

pub(crate) fn validate_migration_snapshot(snapshot: &LegacyScmSnapshot) -> Result<(), String> {
    #[cfg(windows)]
    {
        windows::validate_snapshot_for_restore(snapshot)
    }
    #[cfg(not(windows))]
    {
        let _ = snapshot;
        Err("legacy SCM migration is available only on Windows".to_owned())
    }
}

pub(crate) fn stop_for_migration() -> Result<(), String> {
    #[cfg(windows)]
    {
        windows::migration_stop()
    }
    #[cfg(not(windows))]
    {
        Err("legacy SCM migration is available only on Windows".to_owned())
    }
}

pub(crate) fn remove_for_migration() -> Result<(), String> {
    #[cfg(windows)]
    {
        windows::migration_remove()
    }
    #[cfg(not(windows))]
    {
        Err("legacy SCM migration is available only on Windows".to_owned())
    }
}

pub(crate) fn restore_configuration_after_migration(
    snapshot: &LegacyScmSnapshot,
) -> Result<(), String> {
    #[cfg(windows)]
    {
        windows::migration_restore_configuration(snapshot)
    }
    #[cfg(not(windows))]
    {
        let _ = snapshot;
        Err("legacy SCM migration is available only on Windows".to_owned())
    }
}

pub(crate) fn restore_running_state_after_migration(
    snapshot: &LegacyScmSnapshot,
) -> Result<(), String> {
    #[cfg(windows)]
    {
        windows::migration_restore_running_state(snapshot)
    }
    #[cfg(not(windows))]
    {
        let _ = snapshot;
        Err("legacy SCM migration is available only on Windows".to_owned())
    }
}

/// Whether a legacy "MacType" SCM service exists in any form. A present service —
/// owned, compatible, foreign, or mid-deletion — must not coexist with a freshly
/// started new service (double injection); an inaccessible one is fail-closed.
/// Only a definitively Absent service allows generic new-service activation.
pub(crate) fn legacy_service_present() -> Result<bool, String> {
    match status(false).presence {
        ServicePresence::Absent => Ok(false),
        ServicePresence::Owned
        | ServicePresence::CompatibleUnquoted
        | ServicePresence::Foreign
        | ServicePresence::DeletePending => Ok(true),
        ServicePresence::Inaccessible => Err(
            "a legacy MacType service is present but its state is inaccessible; resolve it before \
             changing the new service"
                .to_owned(),
        ),
    }
}

pub(crate) fn status(registry_conflict: bool) -> LegacyServiceStatus {
    #[cfg(windows)]
    {
        windows::query(registry_conflict)
    }
    #[cfg(not(windows))]
    {
        with_capabilities(
            ServicePresence::Absent,
            ServiceRuntimeState::Unknown,
            None,
            None,
            false,
            registry_conflict,
        )
    }
}

#[cfg(test)]
mod tests;
