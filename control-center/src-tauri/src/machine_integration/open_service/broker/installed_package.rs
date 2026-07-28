use super::{
    super::{
        file_guard::read_bounded_regular_file,
        platform::known_folder,
        runtime::{parse_bundled_runtime_manifest, MAX_BUNDLED_MANIFEST_BYTES},
        INSTALLATION_INCOMPLETE_PREFIX, INSTALLATION_REQUIRED_PREFIX,
        INSTALLATION_UNTRUSTED_PREFIX,
    },
    path_guard::reject_reparse_ancestors,
};
use crate::diagnostics::InstallationPreflightDiagnostics;
use std::{
    ffi::OsString,
    fs,
    os::windows::ffi::OsStringExt,
    path::{Path, PathBuf},
};
use windows_sys::Win32::{
    Foundation::{ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_SUCCESS},
    System::Registry::{RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ, RRF_SUBKEY_WOW6464KEY},
    UI::Shell::FOLDERID_ProgramFiles,
};

const CONTROL_CENTER_FILE: &str = "MacType Control Center.exe";
const SETUP_BROKER_RELATIVE_PATH: &str = r"service-runtime\mactype-service-setup.exe";
const RUNTIME_MANIFEST_RELATIVE_PATH: &str = r"service-runtime\payload\manifest.json";
const INSTALL_RECEIPT_REGISTRY_KEY: &str = r"SOFTWARE\MacType\ControlCenter";
const UNINSTALL_REGISTRY_KEY: &str = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{AF6B9697-3DF2-46C4-B203-79194967AE7A}_is1";
const INSTALL_LOCATION_VALUE: &str = "InstallLocation";
const MAX_INSTALL_LOCATION_BYTES: u32 = 64 * 1024;

#[derive(Clone, Debug)]
pub(in crate::machine_integration::open_service) struct InstalledPackage {
    pub(in crate::machine_integration::open_service) control_center: PathBuf,
    pub(in crate::machine_integration::open_service) setup_broker: PathBuf,
}

#[derive(Clone, Debug)]
pub(in crate::machine_integration::open_service) struct InstallationPreflightFailure {
    pub(in crate::machine_integration::open_service) error: String,
    pub(in crate::machine_integration::open_service) diagnostics:
        Box<InstallationPreflightDiagnostics>,
}

fn path_is_descendant_of(path: &Path, root: &Path) -> bool {
    let path = path.components().collect::<Vec<_>>();
    let root = root.components().collect::<Vec<_>>();
    path.len() > root.len()
        && path.iter().zip(&root).all(|(candidate, expected)| {
            candidate
                .as_os_str()
                .to_string_lossy()
                .eq_ignore_ascii_case(&expected.as_os_str().to_string_lossy())
        })
}

fn diagnostics(current_executable: Option<PathBuf>) -> InstallationPreflightDiagnostics {
    InstallationPreflightDiagnostics {
        expected_installed_control_center: None,
        current_executable: current_executable.map(|path| path.to_string_lossy().into_owned()),
        expected_executable_exists: None,
        installed_control_center: "not-checked".to_owned(),
        setup_broker: "not-checked".to_owned(),
        runtime_manifest: "not-checked".to_owned(),
        runtime_payload: "not-checked".to_owned(),
        elevation_attempted: false,
        machine_state_changed: false,
        rollback_required: false,
    }
}

fn failure(
    prefix: &str,
    message: &str,
    diagnostics: InstallationPreflightDiagnostics,
) -> InstallationPreflightFailure {
    InstallationPreflightFailure {
        error: format!("{prefix} {message}"),
        diagnostics: Box::new(diagnostics),
    }
}

fn required(diagnostics: InstallationPreflightDiagnostics) -> InstallationPreflightFailure {
    failure(
        INSTALLATION_REQUIRED_PREFIX,
        "MacType Control Center is not installed. Service installation and maintenance require \
         the complete installed package. Please run the installer first.",
        diagnostics,
    )
}

fn incomplete(diagnostics: InstallationPreflightDiagnostics) -> InstallationPreflightFailure {
    failure(
        INSTALLATION_INCOMPLETE_PREFIX,
        "The registered MacType Control Center installation is incomplete or damaged. Reinstall \
         the complete package before managing the service.",
        diagnostics,
    )
}

fn untrusted(
    detail: impl std::fmt::Display,
    diagnostics: InstallationPreflightDiagnostics,
) -> InstallationPreflightFailure {
    failure(
        INSTALLATION_UNTRUSTED_PREFIX,
        &format!("The registered MacType Control Center installation is not trusted. {detail}"),
        diagnostics,
    )
}

fn inspect_regular_file(path: &Path) -> Result<bool, String> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(false),
        Err(error) => return Err(error.to_string()),
    };
    if !metadata.is_file() {
        return Err(format!("{} is not a regular file", path.display()));
    }
    reject_reparse_ancestors(path)?;
    Ok(true)
}

fn resolve_installed_package(
    program_files: &Path,
    install_location: Option<&Path>,
    current_executable: Option<PathBuf>,
) -> Result<InstalledPackage, InstallationPreflightFailure> {
    let mut observation = diagnostics(current_executable);
    let Some(install_location) = install_location else {
        observation.installed_control_center = "missing".to_owned();
        return Err(required(observation));
    };

    let expected_control_center = install_location.join(CONTROL_CENTER_FILE);
    observation.expected_installed_control_center =
        Some(expected_control_center.to_string_lossy().into_owned());
    observation.expected_executable_exists = expected_control_center.try_exists().ok();

    let install_metadata = match fs::symlink_metadata(install_location) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            observation.installed_control_center = "missing".to_owned();
            return Err(incomplete(observation));
        }
        Err(error) => {
            observation.installed_control_center = "unknown".to_owned();
            return Err(untrusted(error, observation));
        }
    };
    if !install_metadata.is_dir() {
        observation.installed_control_center = "untrusted".to_owned();
        return Err(untrusted(
            "The registered installation root is not a directory.",
            observation,
        ));
    }
    if let Err(error) = reject_reparse_ancestors(install_location) {
        observation.installed_control_center = "untrusted".to_owned();
        return Err(untrusted(error, observation));
    }

    let canonical_program_files =
        fs::canonicalize(program_files).map_err(|error| untrusted(error, observation.clone()))?;
    let canonical_install_location = fs::canonicalize(install_location)
        .map_err(|error| untrusted(error, observation.clone()))?;
    if !path_is_descendant_of(&canonical_install_location, &canonical_program_files) {
        observation.installed_control_center = "untrusted".to_owned();
        return Err(untrusted(
            "The registered location is outside Program Files.",
            observation,
        ));
    }

    let control_center = canonical_install_location.join(CONTROL_CENTER_FILE);
    let setup_broker = canonical_install_location.join(SETUP_BROKER_RELATIVE_PATH);
    let runtime_manifest = canonical_install_location.join(RUNTIME_MANIFEST_RELATIVE_PATH);
    let control_center_present = match inspect_regular_file(&control_center) {
        Ok(present) => {
            observation.installed_control_center =
                if present { "present" } else { "missing" }.to_owned();
            present
        }
        Err(error) => {
            observation.installed_control_center = "untrusted".to_owned();
            return Err(untrusted(error, observation));
        }
    };
    let setup_present = match inspect_regular_file(&setup_broker) {
        Ok(present) => {
            observation.setup_broker = if present { "present" } else { "missing" }.to_owned();
            present
        }
        Err(error) => {
            observation.setup_broker = "untrusted".to_owned();
            return Err(untrusted(error, observation));
        }
    };
    let manifest_present = match inspect_regular_file(&runtime_manifest) {
        Ok(present) => {
            observation.runtime_manifest = if present { "present" } else { "missing" }.to_owned();
            present
        }
        Err(error) => {
            observation.runtime_manifest = "untrusted".to_owned();
            return Err(untrusted(error, observation));
        }
    };
    if !control_center_present || !setup_present || !manifest_present {
        return Err(incomplete(observation));
    }
    let manifest = match read_bounded_regular_file(
        &runtime_manifest,
        MAX_BUNDLED_MANIFEST_BYTES,
        "installed runtime manifest",
    ) {
        Ok(bytes) => bytes,
        Err(error) => {
            observation.runtime_manifest = "untrusted".to_owned();
            return Err(untrusted(error, observation));
        }
    };
    if parse_bundled_runtime_manifest(&manifest).is_err() {
        observation.runtime_manifest = "invalid".to_owned();
        return Err(incomplete(observation));
    }
    let payload_root = canonical_install_location
        .join("service-runtime")
        .join("payload")
        .join("files");
    for name in mactype_service_contract::IMMUTABLE_RUNTIME_FILES {
        let payload = payload_root.join(name);
        match inspect_regular_file(&payload) {
            Ok(true) => {}
            Ok(false) => {
                observation.runtime_payload = "missing".to_owned();
                return Err(incomplete(observation));
            }
            Err(error) => {
                observation.runtime_payload = "untrusted".to_owned();
                return Err(untrusted(error, observation));
            }
        }
    }
    observation.runtime_payload = "present".to_owned();

    Ok(InstalledPackage {
        control_center,
        setup_broker,
    })
}

#[cfg(test)]
pub(in crate::machine_integration::open_service) fn resolve_installed_package_for_trusted_layout(
    program_files: &Path,
    install_location: Option<&Path>,
) -> Result<PathBuf, String> {
    resolve_installed_package(program_files, install_location, None)
        .map(|package| package.control_center)
        .map_err(|failure| failure.error)
}

pub(in crate::machine_integration::open_service) fn installed_package(
) -> Result<InstalledPackage, InstallationPreflightFailure> {
    let current_executable = std::env::current_exe().ok();
    let mut initial = diagnostics(current_executable.clone());
    let program_files =
        known_folder(&FOLDERID_ProgramFiles).map_err(|error| untrusted(error, initial.clone()))?;
    let install_location = registered_install_location().map_err(|error| {
        initial.installed_control_center = "unknown".to_owned();
        untrusted(error, initial)
    })?;
    resolve_installed_package(
        &program_files,
        install_location.as_deref(),
        current_executable,
    )
}

fn registered_install_location() -> Result<Option<PathBuf>, String> {
    match registered_install_location_from_key(INSTALL_RECEIPT_REGISTRY_KEY)? {
        Some(location) => Ok(Some(location)),
        None => registered_install_location_from_key(UNINSTALL_REGISTRY_KEY),
    }
}

fn registered_install_location_from_key(registry_key: &str) -> Result<Option<PathBuf>, String> {
    let subkey = super::path_guard::wide(registry_key);
    let value = super::path_guard::wide(INSTALL_LOCATION_VALUE);
    let flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    let mut bytes = 0_u32;
    let first = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            subkey.as_ptr(),
            value.as_ptr(),
            flags,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut bytes,
        )
    };
    if matches!(first, ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND) {
        return Ok(None);
    }
    if first != ERROR_SUCCESS
        || !(2..=MAX_INSTALL_LOCATION_BYTES).contains(&bytes)
        || bytes % 2 != 0
    {
        return Err(format!(
            "the installer registration could not be read safely (Win32 {first})"
        ));
    }
    let mut buffer = vec![0_u16; bytes as usize / 2];
    let second = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            subkey.as_ptr(),
            value.as_ptr(),
            flags,
            std::ptr::null_mut(),
            buffer.as_mut_ptr().cast(),
            &mut bytes,
        )
    };
    if second != ERROR_SUCCESS
        || !(2..=MAX_INSTALL_LOCATION_BYTES).contains(&bytes)
        || bytes % 2 != 0
    {
        return Err(format!(
            "the installer registration changed while it was read (Win32 {second})"
        ));
    }
    buffer.truncate(bytes as usize / 2);
    if buffer.last() != Some(&0) {
        return Err("the installer registration is not NUL terminated".to_owned());
    }
    buffer.pop();
    if buffer.is_empty() || buffer.contains(&0) {
        return Err("the installer registration contains an invalid path".to_owned());
    }
    Ok(Some(PathBuf::from(OsString::from_wide(&buffer))))
}
