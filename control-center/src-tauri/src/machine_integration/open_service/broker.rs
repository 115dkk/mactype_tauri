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
            _ => run_setup(action, None),
        }
    }
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
