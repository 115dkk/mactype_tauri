mod file_guard;
mod identity;
mod legacy_status;
mod request;
mod runtime;
mod startup_lifecycle;

#[cfg(test)]
use broker::setup_path_for_trusted_layout;

use file_guard::{read_bounded_regular_file, reject_reparse_chain};
use identity::{
    classify_owned_installation, configured_service_binary, is_protected_service_binary,
    owned_core_service_configuration, same_path, select_service_health, validated_reveal_binary,
    LiveHealthReport, ObservedCoreServiceConfiguration,
};
#[cfg(test)]
use legacy_status::legacy_migration_available;
pub(crate) use legacy_status::{legacy_status, LegacyMacTrayStatus};
pub(crate) use request::SystemServiceAction;
use request::{
    decode_profile_transfer_frame, encode_profile_transfer_frame,
    privileged_request_from_arguments, ProfileTransferToken, BROKER_SWITCH,
    PROFILE_TRANSFER_HEADER_BYTES, PROFILE_TRANSFER_MAGIC, PROFILE_TRANSFER_NONCE_BYTES,
    PROFILE_TRANSFER_SWITCH, PROFILE_TRANSFER_VERSION,
};
#[cfg(test)]
use runtime::bundled_runtime_version;
use runtime::{
    bundled_service_binary, parse_bundled_runtime_manifest, BundledRuntimeManifest,
    MAX_BUNDLED_MANIFEST_BYTES,
};
use startup_lifecycle::{finish_action_with_startup_receipts, StartupReceiptRestorer};

use crate::service_contract::{
    HealthState, InstallationState, RuntimeState, ServiceBackend, SystemServiceStatus,
};
use mactype_service_contract::{GenerationId, HealthReport};
use serde::Serialize;
use std::{
    ffi::OsString,
    fs,
    io::Write,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

pub(crate) fn run_action(
    action: SystemServiceAction,
    profile: Option<&[u8]>,
) -> Result<(), String> {
    if profile.is_some() != action.needs_profile_input()
        || profile.is_some_and(|bytes| {
            bytes.is_empty() || bytes.len() > mactype_service_contract::MAX_PROFILE_BYTES
        })
    {
        return Err("the service action has an invalid profile payload".to_owned());
    }
    let result = {
        #[cfg(windows)]
        {
            windows::run_elevated(action, profile)
        }
        #[cfg(not(windows))]
        {
            let _ = profile;
            Err("system service control is available only on Windows".to_owned())
        }
    };
    finish_action_with_startup_receipts(&mut SystemStartupReceiptRestorer, action, result)
}

struct SystemStartupReceiptRestorer;

impl StartupReceiptRestorer for SystemStartupReceiptRestorer {
    fn restore_local_machine(&mut self) -> Result<(), String> {
        #[cfg(windows)]
        {
            windows::run_elevated(SystemServiceAction::RestoreLegacyTrayAutostart, None)
        }
        #[cfg(not(windows))]
        {
            Err("local-machine startup restoration is available only on Windows".to_owned())
        }
    }

    fn restore_current_user(&mut self) -> Result<(), String> {
        super::legacy_migration::restore_startup_scope(
            super::legacy_migration::StartupReceiptScope::CurrentUser,
        )
    }
}

pub(crate) fn dispatch_privileged_command() -> Option<i32> {
    let request = match privileged_request_from_arguments(std::env::args_os()) {
        Ok(None) => return None,
        Ok(Some(request)) => request,
        Err(_) => return Some(21),
    };
    let result = {
        #[cfg(windows)]
        {
            windows::run_privileged(request.action, request.profile_transfer.as_ref())
        }
        #[cfg(not(windows))]
        {
            let _ = request;
            Err("system service control is available only on Windows".to_owned())
        }
    };
    Some(if result.is_ok() { 0 } else { 21 })
}

pub(crate) fn status() -> SystemServiceStatus {
    #[cfg(windows)]
    {
        windows::query()
    }
    #[cfg(not(windows))]
    {
        absent_status()
    }
}

pub(crate) fn reveal_system_service() -> Result<(), String> {
    #[cfg(windows)]
    {
        windows::reveal_system_service()
    }
    #[cfg(not(windows))]
    {
        Err("system service reveal is available only on Windows".to_owned())
    }
}

fn absent_status() -> SystemServiceStatus {
    SystemServiceStatus {
        backend: ServiceBackend::None,
        installation: InstallationState::Absent,
        runtime: RuntimeState::Stopped,
        health: HealthState::Unknown,
        binary_path: None,
        win32_error: None,
        active_profile_digest: None,
        can_install: true,
        can_remove: false,
        can_start: false,
        can_stop: false,
        can_repair: false,
        can_upgrade: false,
    }
}

use migration::{
    migrate_from_legacy, migration_activation_actions, remove_legacy_after_verification,
    MigrationBackend, MigrationVerification,
};
#[cfg(windows)]
mod broker;
mod migration;
#[cfg(windows)]
mod platform;
#[cfg(windows)]
mod profile_transfer;
#[cfg(windows)]
mod windows;

#[cfg(test)]
mod tests;
