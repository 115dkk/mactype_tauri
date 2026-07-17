mod health;
mod scm;

pub(super) use health::read_health_for_scm_process;
use health::{read_health, read_persisted_health};
use scm::{query_configuration, query_runtime, ServiceHandle};
pub(super) use scm::{running_service_process_id, wide};

use super::{
    windows::{machine_roots, RuntimePointer},
    *,
};
use mactype_service_contract::{HealthState as ContractHealthState, SERVICE_NAME};
use windows_sys::Win32::{
    Foundation::{GetLastError, ERROR_SERVICE_DOES_NOT_EXIST, ERROR_SERVICE_MARKED_FOR_DELETE},
    System::Services::{
        OpenSCManagerW, OpenServiceW, SC_MANAGER_CONNECT, SERVICE_QUERY_CONFIG,
        SERVICE_QUERY_STATUS,
    },
    UI::Shell::{FOLDERID_Windows, SHGetKnownFolderPath},
};

pub(super) fn known_folder(id: *const windows_sys::core::GUID) -> Result<PathBuf, String> {
    let mut value = std::ptr::null_mut();
    let result = unsafe { SHGetKnownFolderPath(id, 0, std::ptr::null_mut(), &mut value) };
    if result < 0 || value.is_null() {
        return Err(format!(
            "SHGetKnownFolderPath failed with HRESULT {result:#x}"
        ));
    }
    let mut length = 0;
    while unsafe { *value.add(length) } != 0 {
        length += 1;
    }
    let path = PathBuf::from(String::from_utf16_lossy(unsafe {
        std::slice::from_raw_parts(value, length)
    }));
    unsafe { windows_sys::Win32::System::Com::CoTaskMemFree(value.cast()) };
    Ok(path)
}

pub(super) fn query() -> SystemServiceStatus {
    let manager = unsafe { OpenSCManagerW(std::ptr::null(), std::ptr::null(), SC_MANAGER_CONNECT) };
    if manager.is_null() {
        return inaccessible(unsafe { GetLastError() }, None);
    }
    let manager = ServiceHandle(manager);
    let service_name = wide(SERVICE_NAME);
    let service = unsafe {
        OpenServiceW(
            manager.0,
            service_name.as_ptr(),
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS,
        )
    };
    if service.is_null() {
        let error = unsafe { GetLastError() };
        return match error {
            ERROR_SERVICE_DOES_NOT_EXIST => absent_status(),
            ERROR_SERVICE_MARKED_FOR_DELETE => SystemServiceStatus {
                backend: ServiceBackend::OpenSource,
                installation: InstallationState::DeletePending,
                runtime: RuntimeState::Unknown,
                win32_error: Some(error),
                can_install: false,
                ..absent_status()
            },
            _ => inaccessible(error, None),
        };
    }
    let service = ServiceHandle(service);
    let configuration = match query_configuration(service.0) {
        Ok(configuration) => configuration,
        Err(error) => return inaccessible(error, None),
    };
    let binary_path = Some(configuration.binary_path.clone());
    let (runtime, service_process_id) = match query_runtime(service.0) {
        Ok(state) => state,
        Err(error) => return inaccessible(error, binary_path),
    };
    let (program_files, _) = match machine_roots() {
        Ok(roots) => roots,
        Err(_) => return inaccessible(3, binary_path),
    };
    let service_root = program_files.join("MacType Control Center").join("Service");
    let expected = current_service_binary(&service_root);
    let bundled = bundled_service_binary(&service_root);
    let configured = configured_service_binary(&configuration.binary_path);
    let protected = configured
        .as_deref()
        .is_some_and(|path| is_protected_service_binary(&service_root, path));
    let owned_configuration = owned_core_service_configuration(&ObservedCoreServiceConfiguration {
        service_type: configuration.service_type,
        start_type: configuration.start_type,
        error_control: configuration.error_control,
        account: &configuration.account,
        display_name: &configuration.display_name,
        load_order_group: &configuration.load_order_group,
        tag_id: configuration.tag_id,
        dependencies_empty: configuration.dependencies.is_empty(),
        protected_image: protected,
    });
    if !owned_configuration {
        return SystemServiceStatus {
            backend: ServiceBackend::Foreign,
            installation: InstallationState::Invalid,
            runtime,
            health: HealthState::Unknown,
            binary_path,
            win32_error: None,
            active_profile_digest: None,
            can_install: false,
            can_remove: false,
            can_start: false,
            can_stop: false,
            can_repair: false,
            can_upgrade: false,
        };
    }
    let installation = match (configured.as_ref(), expected.as_ref(), bundled.as_ref()) {
        (Some(configured), Ok(expected), Ok(bundled)) => {
            classify_owned_installation(configured, expected, bundled)
        }
        (_, Err(_), Ok(_)) => InstallationState::Outdated,
        _ => InstallationState::Invalid,
    };
    let live_report = (runtime == RuntimeState::Running)
        .then(read_health)
        .and_then(Result::ok);
    let persisted_report = read_persisted_health(&service_root).ok();
    let selected =
        select_service_health(runtime, service_process_id, live_report, persisted_report);
    let health = selected
        .as_ref()
        .map(|selected| match selected.report.health {
            ContractHealthState::Unknown => HealthState::Unknown,
            ContractHealthState::Initializing => HealthState::Initializing,
            ContractHealthState::Ready => HealthState::Ready,
            ContractHealthState::Degraded => HealthState::Degraded,
            ContractHealthState::Failed => HealthState::Failed,
        })
        .unwrap_or(HealthState::Unknown);
    let active_profile_digest = selected.and_then(|selected| {
        selected
            .live
            .then_some(selected.report.active_profile_digest)
            .flatten()
    });
    let stable = matches!(runtime, RuntimeState::Running | RuntimeState::Stopped);
    SystemServiceStatus {
        backend: ServiceBackend::OpenSource,
        installation,
        runtime,
        health,
        binary_path,
        win32_error: None,
        active_profile_digest,
        can_install: false,
        can_remove: stable,
        can_start: runtime == RuntimeState::Stopped && installation == InstallationState::Current,
        can_stop: runtime == RuntimeState::Running,
        can_repair: stable && installation == InstallationState::Current,
        can_upgrade: stable && installation == InstallationState::Outdated,
    }
}

fn current_service_binary(service_root: &Path) -> Result<PathBuf, String> {
    let pointer_path = service_root.join("current.json");
    let pointer: RuntimePointer = serde_json::from_slice(&read_bounded_regular_file(
        &pointer_path,
        64 * 1024,
        "protected runtime pointer",
    )?)
    .map_err(|error| error.to_string())?;
    if pointer.schema != 1 || !safe_version(&pointer.version) {
        return Err("invalid protected runtime pointer".to_owned());
    }
    let binary = service_root
        .join("bin")
        .join(pointer.version)
        .join("mactype-service.exe");
    reject_reparse_chain(&binary)?;
    if !binary.is_file() {
        return Err("protected service binary is missing".to_owned());
    }
    Ok(binary)
}

pub(super) fn safe_version(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'+'))
        && !matches!(value, "." | "..")
}

pub(super) fn reveal_system_service() -> Result<(), String> {
    let status = query();
    let (program_files, _) = machine_roots()?;
    let service_root = program_files.join("MacType Control Center").join("Service");
    let binary = validated_reveal_binary(&service_root, &status)?;
    reject_reparse_chain(&binary)?;
    let metadata = fs::metadata(&binary).map_err(|error| error.to_string())?;
    if !metadata.is_file() {
        return Err("the protected system service binary is missing".to_owned());
    }
    let explorer = known_folder(&FOLDERID_Windows)?.join("explorer.exe");
    reject_reparse_chain(&explorer)?;
    if !explorer.is_file() {
        return Err("the fixed Windows Explorer executable is unavailable".to_owned());
    }
    let mut selection = OsString::from("/select,");
    selection.push(&binary);
    Command::new(explorer)
        .arg(selection)
        .spawn()
        .map_err(|error| error.to_string())?;
    Ok(())
}

fn inaccessible(error: u32, binary_path: Option<String>) -> SystemServiceStatus {
    SystemServiceStatus {
        backend: ServiceBackend::None,
        installation: InstallationState::Inaccessible,
        runtime: RuntimeState::Unknown,
        health: HealthState::Unknown,
        binary_path,
        win32_error: Some(error),
        active_profile_digest: None,
        can_install: false,
        can_remove: false,
        can_start: false,
        can_stop: false,
        can_repair: false,
        can_upgrade: false,
    }
}
