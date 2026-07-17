mod model;
mod ownership;
mod restore_contract;

#[cfg(test)]
use model::ServiceTriggerConfiguration;
use model::{require_stable_migration_state, snapshot_trigger_configuration};
pub(crate) use model::{
    FailureAction, FailureActionsConfiguration, LegacyScmSnapshot, SecurityDescriptorSnapshot,
    ServiceConfiguration, ServiceExtendedConfiguration,
};
pub use model::{LegacyServiceStatus, ServicePresence, ServiceRuntimeState};
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
