use super::{
    super::{platform::known_folder, same_path, windows::query, SystemServiceAction},
    path_guard::reject_reparse_ancestors,
};
use crate::service_contract::SystemServiceStatus;
use std::{
    ffi::OsStr,
    fs,
    io::Write,
    path::PathBuf,
    process::{Command, Stdio},
};
use windows_sys::Win32::UI::Shell::FOLDERID_ProgramFiles;

struct OpenServicePublishBackend;

impl crate::machine_integration::MachineBackend for OpenServicePublishBackend {
    fn new_service_status(&mut self) -> SystemServiceStatus {
        query()
    }

    fn appinit_conflict(&mut self) -> Result<bool, String> {
        Ok(crate::machine_integration::registry_conflict_detected())
    }

    fn execute(
        &mut self,
        action: crate::machine_integration::MachineAction,
        profile: Option<&[u8]>,
    ) -> Result<(), String> {
        run_setup(action.into(), profile)
    }
}

pub(super) fn publish_and_activate(profile: &[u8]) -> Result<(), String> {
    if crate::machine_integration::registry_conflict_detected() {
        return Err("AppInit conflicts block machine integration changes".to_owned());
    }
    crate::machine_integration::publish_profile_transaction_with(
        &mut OpenServicePublishBackend,
        profile,
    )
}

pub(in crate::machine_integration::open_service) fn run_setup(
    action: SystemServiceAction,
    profile: Option<&[u8]>,
) -> Result<(), String> {
    let verb = action
        .setup_verb()
        .ok_or_else(|| "the requested action is not a setup broker verb".to_owned())?;
    if profile.is_some() != (action == SystemServiceAction::PublishProfile) {
        return Err("only publish-profile accepts stdin bytes".to_owned());
    }
    run_setup_process(verb, profile)
}

pub(in crate::machine_integration::open_service) fn run_restore_pinned_runtime(
) -> Result<(), String> {
    run_setup_process("restore-runtime", None)
}

fn run_setup_process(verb: &str, profile: Option<&[u8]>) -> Result<(), String> {
    let setup = fixed_setup_path()?;
    let mut command = Command::new(setup);
    command
        .arg(verb)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .stdin(if profile.is_some() {
            Stdio::piped()
        } else {
            Stdio::null()
        });
    let mut child = command.spawn().map_err(|error| error.to_string())?;
    if let Some(bytes) = profile {
        child
            .stdin
            .take()
            .ok_or_else(|| "setup broker stdin is unavailable".to_owned())?
            .write_all(bytes)
            .map_err(|error| error.to_string())?;
    }
    let status = child.wait().map_err(|error| error.to_string())?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("setup broker {verb} failed with {status}"))
    }
}

pub(in crate::machine_integration::open_service) fn fixed_setup_path() -> Result<PathBuf, String> {
    let program_files = known_folder(&FOLDERID_ProgramFiles)?;
    let executable = std::env::current_exe().map_err(|error| error.to_string())?;
    reject_reparse_ancestors(&executable)?;
    let canonical_program_files =
        fs::canonicalize(&program_files).map_err(|error| error.to_string())?;
    let canonical_executable = fs::canonicalize(&executable).map_err(|error| error.to_string())?;
    let expected = setup_path_for_trusted_layout(&canonical_program_files, &canonical_executable)?;
    reject_reparse_ancestors(&expected)?;
    let canonical = fs::canonicalize(&expected).map_err(|error| error.to_string())?;
    let runtime_root = expected
        .parent()
        .ok_or_else(|| "fixed setup broker has no runtime root".to_owned())?;
    if canonical.file_name() != Some(OsStr::new("mactype-service-setup.exe"))
        || canonical.parent() != fs::canonicalize(runtime_root).ok().as_deref()
    {
        return Err("setup broker resolves outside the fixed application layout".to_owned());
    }
    Ok(canonical)
}

pub(in crate::machine_integration::open_service) fn setup_path_for_trusted_layout(
    program_files: &std::path::Path,
    executable: &std::path::Path,
) -> Result<PathBuf, String> {
    let app_root = program_files.join("MacType Control Center");
    let expected_executable = app_root.join("MacType Control Center.exe");
    if !same_path(executable, &expected_executable) {
        return Err("Control Center is outside the fixed Program Files layout".to_owned());
    }
    Ok(app_root
        .join("service-runtime")
        .join("mactype-service-setup.exe"))
}
