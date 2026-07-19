mod elevation;
mod path_guard;
mod process;
mod setup;

pub(super) use elevation::run_elevated;
pub(super) use path_guard::reject_reparse_ancestors;
#[cfg(test)]
pub(super) use process::{
    combine_broker_cleanup_error, terminate_broker_process_with, BrokerProcessControl,
    BrokerTermination,
};
use setup::publish_and_activate;
#[cfg(test)]
pub(super) use setup::setup_path_for_trusted_layout;
pub(super) use setup::{fixed_setup_path, run_restore_pinned_runtime, run_setup};

use super::{
    migrate_from_legacy,
    profile_transfer::{receive_profile_from_pipe_bounded, KillOnCloseJob, PROFILE_PIPE_TIMEOUT},
    remove_legacy_after_verification,
    windows::SystemMigrationBackend,
    ProfileTransferToken, SystemServiceAction,
};

pub(super) fn run_privileged(
    action: SystemServiceAction,
    profile_transfer: Option<&ProfileTransferToken>,
) -> Result<(), String> {
    KillOnCloseJob::new()?.arm_current_process()?;
    if profile_transfer.is_some() != action.needs_profile_input() {
        return Err("the privileged action has invalid profile transfer metadata".to_owned());
    }
    if action.needs_profile_input() {
        let profile = receive_required_profile_bounded(profile_transfer)?;
        run_profile_action(action, &profile)
    } else {
        match action {
            SystemServiceAction::DisableLegacyTrayAutostart => {
                crate::machine_integration::legacy_migration::disable_startup_scope(
                    crate::machine_integration::legacy_migration::StartupReceiptScope::LocalMachine,
                )
            }
            SystemServiceAction::RestoreLegacyTrayAutostart => {
                crate::machine_integration::legacy_migration::restore_startup_scope(
                    crate::machine_integration::legacy_migration::StartupReceiptScope::LocalMachine,
                )
            }
            SystemServiceAction::Install | SystemServiceAction::Start => {
                refuse_conflicting_environment_for_activation()?;
                run_setup(action, None)
            }
            _ => run_setup(action, None),
        }
    }
}

// The unelevated caller already gated these actions, but the UAC consent window
// is an arbitrary interval during which an AppInit entry, a legacy tray process,
// or a legacy SCM service can appear (TOCTOU). Re-validate the conflicting-
// environment gates inside the elevated broker before activating the new service.
// The migration path drives its own run_setup steps directly and never reaches
// this arm, so it is unaffected.
fn refuse_conflicting_environment_for_activation() -> Result<(), String> {
    use crate::machine_integration::{legacy_mactray, registry_conflict_detected};
    if registry_conflict_detected() {
        return Err("AppInit conflicts block this service change".to_owned());
    }
    if legacy_mactray::tray_status().blocks_machine_change() {
        return Err("the legacy MacTray tray mode blocks this service change".to_owned());
    }
    if legacy_mactray::legacy_service_present()? {
        return Err(
            "a legacy MacType service is still installed; migrate it before starting the new service"
                .to_owned(),
        );
    }
    Ok(())
}

fn receive_required_profile_bounded(
    profile_transfer: Option<&ProfileTransferToken>,
) -> Result<Vec<u8>, String> {
    let token = profile_transfer
        .ok_or_else(|| "the privileged action requires profile transfer metadata".to_owned())?;
    receive_profile_from_pipe_bounded(token, PROFILE_PIPE_TIMEOUT)
}

fn run_profile_action(action: SystemServiceAction, profile: &[u8]) -> Result<(), String> {
    match action {
        SystemServiceAction::PublishProfile => publish_and_activate(profile),
        SystemServiceAction::MigrateFromLegacy => {
            migrate_from_legacy(&mut SystemMigrationBackend::default(), profile)
        }
        SystemServiceAction::RemoveLegacy => {
            remove_legacy_after_verification(&mut SystemMigrationBackend::default(), profile)
        }
        _ => Err("the privileged action does not accept profile input".to_owned()),
    }
}
