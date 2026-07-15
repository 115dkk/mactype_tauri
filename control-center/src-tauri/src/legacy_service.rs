use serde::Serialize;
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum ServicePresence {
    Absent,
    Owned,
    CompatibleUnquoted,
    Foreign,
    DeletePending,
    Inaccessible,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum ServiceRuntimeState {
    Stopped,
    StartPending,
    StopPending,
    Running,
    ContinuePending,
    PausePending,
    Paused,
    Unknown,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LegacyServiceStatus {
    pub presence: ServicePresence,
    pub state: ServiceRuntimeState,
    pub binary_path: Option<String>,
    pub win32_error: Option<u32>,
    pub trusted_binary_available: bool,
    pub registry_conflict: bool,
    pub can_install: bool,
    pub can_remove: bool,
    pub can_start: bool,
    pub can_stop: bool,
}

#[derive(Clone, Debug)]
struct ServiceConfiguration {
    binary_path: String,
    service_type: u32,
    start_type: u32,
    error_control: u32,
    account: String,
    dependencies: Vec<String>,
}

fn service_command_path(path: &Path) -> String {
    let path = path.to_string_lossy();
    path.strip_prefix(r"\\?\")
        .unwrap_or(path.as_ref())
        .to_owned()
}

fn classify_configuration(configuration: &ServiceConfiguration, trusted: &Path) -> ServicePresence {
    const SERVICE_WIN32_OWN_PROCESS: u32 = 0x10;
    const SERVICE_AUTO_START: u32 = 2;
    const SERVICE_ERROR_NORMAL: u32 = 1;

    if configuration.service_type != SERVICE_WIN32_OWN_PROCESS
        || configuration.start_type != SERVICE_AUTO_START
        || configuration.error_control != SERVICE_ERROR_NORMAL
        || !configuration.account.eq_ignore_ascii_case("LocalSystem")
        || !configuration
            .dependencies
            .iter()
            .any(|dependency| dependency.eq_ignore_ascii_case("winmgmt"))
    {
        return ServicePresence::Foreign;
    }
    // `std::fs::canonicalize` returns an extended-length (`\\?\`) path on
    // Windows, while the Service Control Manager normally returns ImagePath
    // without that prefix. Both strings identify the same trusted executable.
    let trusted = service_command_path(trusted);
    let quoted = format!("\"{trusted}\" -service");
    let unquoted = format!("{trusted} -service");
    let actual = configuration.binary_path.trim();
    if actual.eq_ignore_ascii_case(&quoted) {
        ServicePresence::Owned
    } else if actual.eq_ignore_ascii_case(&unquoted) {
        ServicePresence::CompatibleUnquoted
    } else {
        ServicePresence::Foreign
    }
}

fn with_capabilities(
    presence: ServicePresence,
    state: ServiceRuntimeState,
    binary_path: Option<String>,
    win32_error: Option<u32>,
    trusted_binary_available: bool,
    registry_conflict: bool,
) -> LegacyServiceStatus {
    let owned = matches!(
        presence,
        ServicePresence::Owned | ServicePresence::CompatibleUnquoted
    );
    LegacyServiceStatus {
        presence,
        state,
        binary_path,
        win32_error,
        trusted_binary_available,
        registry_conflict,
        can_install: presence == ServicePresence::Absent
            && trusted_binary_available
            && !registry_conflict,
        can_remove: owned && !registry_conflict,
        can_start: owned && state == ServiceRuntimeState::Stopped && !registry_conflict,
        can_stop: owned && state == ServiceRuntimeState::Running && !registry_conflict,
    }
}

fn is_trusted_mactray_layout(program_files: &Path, candidate: &Path) -> bool {
    let Ok(relative) = candidate.strip_prefix(program_files) else {
        return false;
    };
    let mut components = relative.components();
    let Some(mactype) = components.next() else {
        return false;
    };
    let Some(mactray) = components.next() else {
        return false;
    };
    components.next().is_none()
        && mactype
            .as_os_str()
            .to_string_lossy()
            .eq_ignore_ascii_case("MacType")
        && mactray
            .as_os_str()
            .to_string_lossy()
            .eq_ignore_ascii_case("MacTray.exe")
}

fn privileged_action_from_arguments(
    arguments: impl IntoIterator<Item = OsString>,
) -> Result<Option<&'static str>, String> {
    let mut arguments = arguments.into_iter();
    let _executable = arguments.next();
    let arguments = arguments.collect::<Vec<_>>();
    let broker_flag = OsStr::new("--legacy-service-broker");

    if !arguments.iter().any(|argument| argument == broker_flag) {
        return Ok(None);
    }
    if arguments.len() != 2 || arguments[0] != broker_flag {
        return Err("invalid legacy service broker invocation".to_owned());
    }
    match arguments[1].to_str() {
        Some("install") => Ok(Some("install")),
        Some("remove") => Ok(Some("remove")),
        Some("start") => Ok(Some("start")),
        Some("stop") => Ok(Some("stop")),
        _ => Err("unsupported legacy service action".to_owned()),
    }
}

#[cfg(windows)]
mod windows {
    use super::*;
    use std::{ffi::OsStr, os::windows::ffi::OsStrExt, process::Command, thread, time::Duration};
    use windows_sys::Win32::{
        Foundation::{
            CloseHandle, GetLastError, ERROR_CANCELLED, ERROR_INSUFFICIENT_BUFFER,
            ERROR_SERVICE_ALREADY_RUNNING, ERROR_SERVICE_DOES_NOT_EXIST,
            ERROR_SERVICE_MARKED_FOR_DELETE, ERROR_SERVICE_NOT_ACTIVE, HANDLE, WAIT_OBJECT_0,
        },
        System::{
            Com::CoTaskMemFree,
            Services::{
                CloseServiceHandle, ControlService, OpenSCManagerW, OpenServiceW,
                QueryServiceConfigW, QueryServiceStatusEx, StartServiceW, QUERY_SERVICE_CONFIGW,
                SC_HANDLE, SC_MANAGER_CONNECT, SC_STATUS_PROCESS_INFO, SERVICE_CONTINUE_PENDING,
                SERVICE_CONTROL_STOP, SERVICE_PAUSED, SERVICE_PAUSE_PENDING, SERVICE_QUERY_CONFIG,
                SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_START, SERVICE_START_PENDING,
                SERVICE_STATUS, SERVICE_STATUS_PROCESS, SERVICE_STOP, SERVICE_STOPPED,
                SERVICE_STOP_PENDING,
            },
            Threading::{GetExitCodeProcess, WaitForSingleObject, INFINITE},
        },
        UI::Shell::{
            FOLDERID_ProgramFiles, SHGetKnownFolderPath, ShellExecuteExW, SEE_MASK_NOCLOSEPROCESS,
            SHELLEXECUTEINFOW,
        },
    };

    struct ServiceHandle(SC_HANDLE);

    impl Drop for ServiceHandle {
        fn drop(&mut self) {
            unsafe { CloseServiceHandle(self.0) };
        }
    }

    fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
        value.as_ref().encode_wide().chain(Some(0)).collect()
    }

    unsafe fn wide_string(pointer: *const u16) -> String {
        if pointer.is_null() {
            return String::new();
        }
        let mut length = 0;
        while unsafe { *pointer.add(length) } != 0 {
            length += 1;
        }
        String::from_utf16_lossy(unsafe { std::slice::from_raw_parts(pointer, length) })
    }

    unsafe fn multi_string(pointer: *const u16) -> Vec<String> {
        let mut result = Vec::new();
        if pointer.is_null() {
            return result;
        }
        let mut offset = 0;
        loop {
            let start = unsafe { pointer.add(offset) };
            if unsafe { *start } == 0 {
                break;
            }
            let value = unsafe { wide_string(start) };
            offset += value.encode_utf16().count() + 1;
            result.push(value);
        }
        result
    }

    pub(super) fn trusted_mactray_path() -> Option<PathBuf> {
        let mut pointer = std::ptr::null_mut();
        let result = unsafe {
            SHGetKnownFolderPath(
                &FOLDERID_ProgramFiles,
                0,
                std::ptr::null_mut(),
                &mut pointer,
            )
        };
        if result < 0 || pointer.is_null() {
            return None;
        }
        let root = unsafe { wide_string(pointer) };
        unsafe { CoTaskMemFree(pointer.cast()) };
        let root = std::fs::canonicalize(root).ok()?;
        let candidate = std::fs::canonicalize(root.join("MacType").join("MacTray.exe")).ok()?;
        (candidate.is_file() && is_trusted_mactray_layout(&root, &candidate)).then_some(candidate)
    }

    fn runtime_state(raw: u32) -> ServiceRuntimeState {
        match raw {
            SERVICE_STOPPED => ServiceRuntimeState::Stopped,
            SERVICE_START_PENDING => ServiceRuntimeState::StartPending,
            SERVICE_STOP_PENDING => ServiceRuntimeState::StopPending,
            SERVICE_RUNNING => ServiceRuntimeState::Running,
            SERVICE_CONTINUE_PENDING => ServiceRuntimeState::ContinuePending,
            SERVICE_PAUSE_PENDING => ServiceRuntimeState::PausePending,
            SERVICE_PAUSED => ServiceRuntimeState::Paused,
            _ => ServiceRuntimeState::Unknown,
        }
    }

    fn inaccessible(code: u32, trusted: bool, registry: bool) -> LegacyServiceStatus {
        with_capabilities(
            ServicePresence::Inaccessible,
            ServiceRuntimeState::Unknown,
            None,
            Some(code),
            trusted,
            registry,
        )
    }

    pub(super) fn query(registry_conflict: bool) -> LegacyServiceStatus {
        let trusted = trusted_mactray_path();
        let trusted_available = trusted.is_some();
        let manager =
            unsafe { OpenSCManagerW(std::ptr::null(), std::ptr::null(), SC_MANAGER_CONNECT) };
        if manager.is_null() {
            return inaccessible(
                unsafe { GetLastError() },
                trusted_available,
                registry_conflict,
            );
        }
        let manager = ServiceHandle(manager);
        let name = wide("MacType");
        let service = unsafe {
            OpenServiceW(
                manager.0,
                name.as_ptr(),
                SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
            )
        };
        if service.is_null() {
            let code = unsafe { GetLastError() };
            let presence = match code {
                ERROR_SERVICE_DOES_NOT_EXIST => ServicePresence::Absent,
                ERROR_SERVICE_MARKED_FOR_DELETE => ServicePresence::DeletePending,
                _ => ServicePresence::Inaccessible,
            };
            return with_capabilities(
                presence,
                ServiceRuntimeState::Unknown,
                None,
                (presence == ServicePresence::Inaccessible).then_some(code),
                trusted_available,
                registry_conflict,
            );
        }
        let service = ServiceHandle(service);
        let mut process_status = SERVICE_STATUS_PROCESS::default();
        let mut needed = 0;
        if unsafe {
            QueryServiceStatusEx(
                service.0,
                SC_STATUS_PROCESS_INFO,
                (&mut process_status as *mut SERVICE_STATUS_PROCESS).cast(),
                std::mem::size_of::<SERVICE_STATUS_PROCESS>() as u32,
                &mut needed,
            )
        } == 0
        {
            return inaccessible(
                unsafe { GetLastError() },
                trusted_available,
                registry_conflict,
            );
        }

        let initial_query =
            unsafe { QueryServiceConfigW(service.0, std::ptr::null_mut(), 0, &mut needed) };
        let initial_error = unsafe { GetLastError() };
        if initial_query != 0
            || initial_error != ERROR_INSUFFICIENT_BUFFER
            || needed < std::mem::size_of::<QUERY_SERVICE_CONFIGW>() as u32
        {
            return inaccessible(initial_error, trusted_available, registry_conflict);
        }
        let word_size = std::mem::size_of::<usize>();
        let mut buffer = vec![0usize; (needed as usize).div_ceil(word_size)];
        let configuration = buffer.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>();
        if unsafe { QueryServiceConfigW(service.0, configuration, needed, &mut needed) } == 0 {
            return inaccessible(
                unsafe { GetLastError() },
                trusted_available,
                registry_conflict,
            );
        }
        let raw = unsafe { &*configuration };
        let configuration = ServiceConfiguration {
            binary_path: unsafe { wide_string(raw.lpBinaryPathName) },
            service_type: raw.dwServiceType,
            start_type: raw.dwStartType,
            error_control: raw.dwErrorControl,
            account: unsafe { wide_string(raw.lpServiceStartName) },
            dependencies: unsafe { multi_string(raw.lpDependencies) },
        };
        let presence = trusted
            .as_deref()
            .map(|path| classify_configuration(&configuration, path))
            .unwrap_or(ServicePresence::Foreign);
        with_capabilities(
            presence,
            runtime_state(process_status.dwCurrentState),
            Some(configuration.binary_path),
            None,
            trusted_available,
            registry_conflict,
        )
    }

    fn open_for(access: u32) -> Result<ServiceHandle, u32> {
        let manager =
            unsafe { OpenSCManagerW(std::ptr::null(), std::ptr::null(), SC_MANAGER_CONNECT) };
        if manager.is_null() {
            return Err(unsafe { GetLastError() });
        }
        let manager = ServiceHandle(manager);
        let name = wide("MacType");
        let service = unsafe { OpenServiceW(manager.0, name.as_ptr(), access) };
        if service.is_null() {
            Err(unsafe { GetLastError() })
        } else {
            Ok(ServiceHandle(service))
        }
    }

    fn wait_for(target: ServiceRuntimeState) -> Result<(), String> {
        for _ in 0..120 {
            let status = query(false);
            if status.state == target
                || (target == ServiceRuntimeState::Stopped
                    && status.presence == ServicePresence::Absent)
            {
                return Ok(());
            }
            if matches!(
                status.presence,
                ServicePresence::Foreign | ServicePresence::Inaccessible
            ) {
                return Err("legacy service changed to an unsafe state".to_owned());
            }
            thread::sleep(Duration::from_millis(250));
        }
        Err("legacy service operation timed out after 30 seconds".to_owned())
    }

    fn wait_until_absent() -> Result<(), String> {
        for _ in 0..120 {
            let status = query(crate::execution::registry_mode_detected());
            match status.presence {
                ServicePresence::Absent => return Ok(()),
                ServicePresence::Owned
                | ServicePresence::CompatibleUnquoted
                | ServicePresence::DeletePending => {}
                ServicePresence::Foreign | ServicePresence::Inaccessible => {
                    return Err("legacy service changed to an unsafe state".to_owned());
                }
            }
            thread::sleep(Duration::from_millis(250));
        }
        Err("legacy service removal timed out after 30 seconds".to_owned())
    }

    fn start() -> Result<(), String> {
        let service = open_for(SERVICE_START | SERVICE_QUERY_STATUS)
            .map_err(|code| format!("OpenServiceW failed with {code}"))?;
        if unsafe { StartServiceW(service.0, 0, std::ptr::null()) } == 0 {
            let code = unsafe { GetLastError() };
            if code != ERROR_SERVICE_ALREADY_RUNNING {
                return Err(format!("StartServiceW failed with {code}"));
            }
        }
        drop(service);
        wait_for(ServiceRuntimeState::Running)
    }

    fn stop() -> Result<(), String> {
        let service = open_for(SERVICE_STOP | SERVICE_QUERY_STATUS)
            .map_err(|code| format!("OpenServiceW failed with {code}"))?;
        let mut status = SERVICE_STATUS::default();
        if unsafe { ControlService(service.0, SERVICE_CONTROL_STOP, &mut status) } == 0 {
            let code = unsafe { GetLastError() };
            if code != ERROR_SERVICE_NOT_ACTIVE {
                return Err(format!("ControlService failed with {code}"));
            }
        }
        drop(service);
        wait_for(ServiceRuntimeState::Stopped)
    }

    fn run_mactray(argument: &str) -> Result<(), String> {
        let path = trusted_mactray_path()
            .ok_or_else(|| "trusted Program Files MacTray.exe was not found".to_owned())?;
        let status = Command::new(path)
            .args([argument, "/SILENT"])
            .status()
            .map_err(|error| error.to_string())?;
        if status.success() {
            Ok(())
        } else {
            Err(format!("MacTray {argument} exited with {status}"))
        }
    }

    pub(super) fn privileged_mutate(action: &str) -> Result<(), String> {
        let before = query(crate::execution::registry_mode_detected());
        match action {
            "install" => {
                if !before.can_install {
                    return Err(
                        "legacy service installation is not safe in the current state".to_owned(),
                    );
                }
                run_mactray("/INSTALL")?;
                let installed = query(false);
                if !matches!(
                    installed.presence,
                    ServicePresence::Owned | ServicePresence::CompatibleUnquoted
                ) {
                    return Err("MacTray did not install an owned MacType service".to_owned());
                }
                if installed.state != ServiceRuntimeState::Running {
                    start()?;
                }
                wait_for(ServiceRuntimeState::Running)
            }
            "remove" => {
                if !before.can_remove {
                    return Err("only an owned MacType service can be removed".to_owned());
                }
                if before.state != ServiceRuntimeState::Stopped {
                    stop()?;
                }
                run_mactray("/UNINSTALL")?;
                wait_until_absent()
            }
            "start" => {
                if !before.can_start {
                    return Err("legacy service cannot be started in the current state".to_owned());
                }
                start()
            }
            "stop" => {
                if !before.can_stop {
                    return Err("legacy service cannot be stopped in the current state".to_owned());
                }
                stop()
            }
            _ => Err("unsupported legacy service action".to_owned()),
        }
    }

    pub(super) fn run_elevated(action: &str) -> Result<(), String> {
        if !matches!(action, "install" | "remove" | "start" | "stop") {
            return Err("unsupported legacy service action".to_owned());
        }
        let executable = std::env::current_exe().map_err(|error| error.to_string())?;
        let executable = wide(executable.as_os_str());
        let verb = wide("runas");
        let parameters = wide(format!("--legacy-service-broker {action}"));
        let mut info = SHELLEXECUTEINFOW {
            cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
            fMask: SEE_MASK_NOCLOSEPROCESS,
            lpVerb: verb.as_ptr(),
            lpFile: executable.as_ptr(),
            lpParameters: parameters.as_ptr(),
            nShow: 0,
            ..Default::default()
        };
        if unsafe { ShellExecuteExW(&mut info) } == 0 {
            let code = unsafe { GetLastError() };
            return if code == ERROR_CANCELLED {
                Err("administrator approval was cancelled".to_owned())
            } else {
                Err(format!("ShellExecuteExW failed with {code}"))
            };
        }
        if info.hProcess.is_null() {
            return Err("elevated service broker did not return a process handle".to_owned());
        }
        let process: HANDLE = info.hProcess;
        let wait = unsafe { WaitForSingleObject(process, INFINITE) };
        if wait != WAIT_OBJECT_0 {
            unsafe { CloseHandle(process) };
            return Err(format!(
                "waiting for the elevated broker failed with {wait}"
            ));
        }
        let mut exit_code = 0;
        let ok = unsafe { GetExitCodeProcess(process, &mut exit_code) };
        unsafe { CloseHandle(process) };
        if ok == 0 {
            return Err("could not read the elevated broker exit code".to_owned());
        }
        if exit_code == 0 {
            Ok(())
        } else {
            Err(format!(
                "legacy service broker failed with exit code {exit_code}"
            ))
        }
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

#[tauri::command]
pub(crate) fn manage_legacy_service(action: String) -> Result<LegacyServiceStatus, String> {
    #[cfg(windows)]
    {
        windows::run_elevated(&action)?;
        Ok(status(crate::execution::registry_mode_detected()))
    }
    #[cfg(not(windows))]
    {
        let _ = action;
        Err("legacy service control is available only on Windows".to_owned())
    }
}

pub(crate) fn dispatch_privileged_command() -> Option<i32> {
    let action = match privileged_action_from_arguments(std::env::args_os()) {
        Ok(None) => return None,
        Ok(Some(action)) => action,
        Err(_) => return Some(20),
    };
    let result = {
        #[cfg(windows)]
        {
            windows::privileged_mutate(action)
        }
        #[cfg(not(windows))]
        {
            let _ = action;
            Err("legacy service control is available only on Windows".to_owned())
        }
    };
    Some(if result.is_ok() { 0 } else { 20 })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn official_configuration(path: &Path) -> ServiceConfiguration {
        ServiceConfiguration {
            binary_path: format!("\"{}\" -service", path.display()),
            service_type: 0x10,
            start_type: 2,
            error_control: 1,
            account: "LocalSystem".to_owned(),
            dependencies: vec!["winmgmt".to_owned()],
        }
    }

    #[test]
    fn only_the_verified_mactray_service_is_owned() {
        let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
        assert_eq!(
            classify_configuration(&official_configuration(path), path),
            ServicePresence::Owned
        );
        let mut foreign = official_configuration(path);
        foreign.binary_path = r"C:\Temp\MacTray.exe -service".to_owned();
        assert_eq!(
            classify_configuration(&foreign, path),
            ServicePresence::Foreign
        );
    }

    #[test]
    fn compatible_unquoted_official_service_is_a_warning_not_foreign() {
        let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
        let mut configuration = official_configuration(path);
        configuration.binary_path = format!("{} -service", path.display());
        assert_eq!(
            classify_configuration(&configuration, path),
            ServicePresence::CompatibleUnquoted
        );
        let status = with_capabilities(
            ServicePresence::CompatibleUnquoted,
            ServiceRuntimeState::Stopped,
            Some(configuration.binary_path),
            None,
            true,
            false,
        );
        assert!(status.can_start);
        assert!(status.can_remove);
        assert!(!status.can_install);
        assert!(!status.can_stop);
    }

    #[test]
    fn canonicalized_windows_path_matches_the_service_manager_image_path() {
        let canonical = Path::new(r"\\?\C:\Program Files\MacType\MacTray.exe");
        let mut configuration =
            official_configuration(Path::new(r"C:\Program Files\MacType\MacTray.exe"));

        assert_eq!(
            classify_configuration(&configuration, canonical),
            ServicePresence::Owned
        );

        configuration.binary_path = r"C:\Program Files\MacType\MacTray.exe -service".to_owned();
        let presence = classify_configuration(&configuration, canonical);
        assert_eq!(presence, ServicePresence::CompatibleUnquoted);

        let status = with_capabilities(
            presence,
            ServiceRuntimeState::Running,
            Some(configuration.binary_path),
            None,
            true,
            false,
        );
        assert!(status.can_remove);
        assert!(status.can_stop);
    }

    #[test]
    fn registry_conflict_blocks_install_and_start() {
        let absent = with_capabilities(
            ServicePresence::Absent,
            ServiceRuntimeState::Unknown,
            None,
            None,
            true,
            true,
        );
        assert!(!absent.can_install);
        let stopped = with_capabilities(
            ServicePresence::Owned,
            ServiceRuntimeState::Stopped,
            None,
            None,
            true,
            true,
        );
        assert!(!stopped.can_start);
        assert!(!stopped.can_remove);
    }

    fn assert_no_mutation(status: &LegacyServiceStatus) {
        assert!(!status.can_install);
        assert!(!status.can_remove);
        assert!(!status.can_start);
        assert!(!status.can_stop);
    }

    #[test]
    fn unsafe_service_states_never_expose_mutation_capabilities() {
        for presence in [
            ServicePresence::Foreign,
            ServicePresence::DeletePending,
            ServicePresence::Inaccessible,
        ] {
            for state in [
                ServiceRuntimeState::Stopped,
                ServiceRuntimeState::Running,
                ServiceRuntimeState::Unknown,
            ] {
                let status = with_capabilities(presence, state, None, None, true, false);
                assert_no_mutation(&status);
            }
        }
    }

    #[test]
    fn registry_conflict_blocks_every_mutation() {
        for presence in [
            ServicePresence::Absent,
            ServicePresence::Owned,
            ServicePresence::CompatibleUnquoted,
        ] {
            for state in [ServiceRuntimeState::Stopped, ServiceRuntimeState::Running] {
                let status = with_capabilities(presence, state, None, None, true, true);
                assert_no_mutation(&status);
            }
        }
    }

    #[test]
    fn service_metadata_must_match_the_official_configuration() {
        let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
        let mut configurations = Vec::new();

        let mut wrong_type = official_configuration(path);
        wrong_type.service_type = 0x20;
        configurations.push(wrong_type);

        let mut manual_start = official_configuration(path);
        manual_start.start_type = 3;
        configurations.push(manual_start);

        let mut wrong_error_control = official_configuration(path);
        wrong_error_control.error_control = 0;
        configurations.push(wrong_error_control);

        let mut wrong_account = official_configuration(path);
        wrong_account.account = "LocalService".to_owned();
        configurations.push(wrong_account);

        let mut missing_dependency = official_configuration(path);
        missing_dependency.dependencies.clear();
        configurations.push(missing_dependency);

        let mut extra_argument = official_configuration(path);
        extra_argument.binary_path.push_str(" unexpected");
        configurations.push(extra_argument);

        for configuration in configurations {
            assert_eq!(
                classify_configuration(&configuration, path),
                ServicePresence::Foreign
            );
        }
    }

    #[test]
    fn trusted_binary_must_resolve_to_the_exact_program_files_layout() {
        let root = Path::new("program-files");
        assert!(is_trusted_mactray_layout(
            root,
            &root.join("MacType").join("MacTray.exe")
        ));
        assert!(is_trusted_mactray_layout(
            root,
            &root.join("mactype").join("MACTRAY.EXE")
        ));
        assert!(!is_trusted_mactray_layout(
            root,
            &root.join("MacType-old").join("MacTray.exe")
        ));
        assert!(!is_trusted_mactray_layout(
            root,
            &root.join("MacType").join("bin").join("MacTray.exe")
        ));
        assert!(!is_trusted_mactray_layout(
            root,
            &Path::new("other-root").join("MacType").join("MacTray.exe")
        ));
    }

    fn broker_arguments(arguments: &[&str]) -> Result<Option<&'static str>, String> {
        privileged_action_from_arguments(arguments.iter().map(OsString::from))
    }

    #[test]
    fn privileged_broker_accepts_only_an_exact_action_invocation() {
        for action in ["install", "remove", "start", "stop"] {
            assert_eq!(
                broker_arguments(&["control-center.exe", "--legacy-service-broker", action])
                    .unwrap(),
                Some(action)
            );
        }

        assert_eq!(broker_arguments(&["control-center.exe"]).unwrap(), None);
        assert!(broker_arguments(&[
            "control-center.exe",
            "--legacy-service-broker",
            "start",
            "unexpected"
        ])
        .is_err());
        assert!(broker_arguments(&[
            "control-center.exe",
            "--tray",
            "--legacy-service-broker",
            "start"
        ])
        .is_err());
        assert!(
            broker_arguments(&["control-center.exe", "--legacy-service-broker", "restart"])
                .is_err()
        );
    }
}
