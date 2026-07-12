use serde::Serialize;
use std::{env, path::Path, process::Command};

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ExecutionStatus {
    pub tray_available: bool,
    pub auto_start: bool,
    pub manual_launcher_available: bool,
    pub legacy_service_detected: bool,
    pub legacy_service_running: bool,
    pub registry_mode_detected: bool,
    pub system_modes_supported: bool,
    pub system_mode_note: String,
}

pub fn status(installation_root: Option<&Path>) -> ExecutionStatus {
    let (legacy_service_detected, legacy_service_running) = service_status();
    ExecutionStatus {
        tray_available: true,
        auto_start: autostart_value().is_some(),
        manual_launcher_available: installation_root
            .is_some_and(|root| root.join("MacLoader.exe").is_file()),
        legacy_service_detected,
        legacy_service_running,
        registry_mode_detected: registry_mode_detected(),
        system_modes_supported: false,
        system_mode_note: "기존 MacTray 서비스는 비공개 Delphi 실행 파일이므로 제어하지 않습니다. AppInit 레지스트리 모드는 공식 프로젝트가 부팅 장애 위험 때문에 제거했으므로 읽기 전용으로만 감지합니다.".to_owned(),
    }
}

pub fn set_autostart(enabled: bool) -> Result<bool, String> {
    set_autostart_impl(enabled)?;
    Ok(autostart_value().is_some())
}

pub fn launch_with_mactype(
    installation_root: &Path,
    target: &str,
    arguments: &[String],
) -> Result<u32, String> {
    if arguments.len() > 32 || arguments.iter().any(|argument| argument.len() > 4096) {
        return Err(
            "manual launch accepts at most 32 arguments of 4096 characters each".to_owned(),
        );
    }
    let target = std::fs::canonicalize(target).map_err(|error| error.to_string())?;
    if !target.is_file()
        || !target
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("exe"))
    {
        return Err("manual launch target must be an existing .exe file".to_owned());
    }
    let loader = installation_root.join("MacLoader.exe");
    if !loader.is_file() {
        return Err("MacLoader.exe was not found in the selected installation".to_owned());
    }
    Command::new(loader)
        .arg(&target)
        .args(arguments)
        .current_dir(installation_root)
        .spawn()
        .map(|child| child.id())
        .map_err(|error| error.to_string())
}

#[cfg(windows)]
fn wide(value: &str) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    std::ffi::OsStr::new(value)
        .encode_wide()
        .chain(Some(0))
        .collect()
}

#[cfg(windows)]
fn read_registry_string(
    root: windows_sys::Win32::System::Registry::HKEY,
    subkey: &str,
    value: &str,
    flags: u32,
) -> Option<String> {
    use windows_sys::Win32::{Foundation::ERROR_SUCCESS, System::Registry::RegGetValueW};
    let subkey = wide(subkey);
    let value = wide(value);
    let mut bytes = 0u32;
    let result = unsafe {
        RegGetValueW(
            root,
            subkey.as_ptr(),
            value.as_ptr(),
            flags,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut bytes,
        )
    };
    if result != ERROR_SUCCESS || bytes < 2 {
        return None;
    }
    let mut buffer = vec![0u16; bytes as usize / 2];
    let result = unsafe {
        RegGetValueW(
            root,
            subkey.as_ptr(),
            value.as_ptr(),
            flags,
            std::ptr::null_mut(),
            buffer.as_mut_ptr().cast(),
            &mut bytes,
        )
    };
    if result != ERROR_SUCCESS {
        return None;
    }
    let length = buffer
        .iter()
        .position(|unit| *unit == 0)
        .unwrap_or(buffer.len());
    Some(String::from_utf16_lossy(&buffer[..length]))
}

#[cfg(windows)]
fn autostart_value() -> Option<String> {
    use windows_sys::Win32::System::Registry::{HKEY_CURRENT_USER, RRF_RT_REG_SZ};
    read_registry_string(
        HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        "MacTypeControlCenter",
        RRF_RT_REG_SZ,
    )
}

#[cfg(not(windows))]
fn autostart_value() -> Option<String> {
    None
}

#[cfg(windows)]
fn set_autostart_impl(enabled: bool) -> Result<(), String> {
    use windows_sys::Win32::{
        Foundation::{ERROR_FILE_NOT_FOUND, ERROR_SUCCESS},
        System::Registry::{RegDeleteKeyValueW, RegSetKeyValueW, HKEY_CURRENT_USER, REG_SZ},
    };
    let subkey = wide("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    let name = wide("MacTypeControlCenter");
    let result = if enabled {
        let executable = env::current_exe().map_err(|error| error.to_string())?;
        let command = wide(&format!("\"{}\" --tray", executable.display()));
        unsafe {
            RegSetKeyValueW(
                HKEY_CURRENT_USER,
                subkey.as_ptr(),
                name.as_ptr(),
                REG_SZ,
                command.as_ptr().cast(),
                (command.len() * 2) as u32,
            )
        }
    } else {
        unsafe { RegDeleteKeyValueW(HKEY_CURRENT_USER, subkey.as_ptr(), name.as_ptr()) }
    };
    if result == ERROR_SUCCESS || (!enabled && result == ERROR_FILE_NOT_FOUND) {
        Ok(())
    } else {
        Err(format!(
            "autostart registry update failed with Windows error {result}"
        ))
    }
}

#[cfg(not(windows))]
fn set_autostart_impl(_enabled: bool) -> Result<(), String> {
    Err("autostart is supported only on Windows".to_owned())
}

#[cfg(windows)]
fn registry_mode_detected() -> bool {
    use windows_sys::Win32::System::Registry::{
        HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ, RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY,
    };
    let key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows";
    [RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY]
        .into_iter()
        .filter_map(|view| {
            read_registry_string(
                HKEY_LOCAL_MACHINE,
                key,
                "AppInit_DLLs",
                RRF_RT_REG_SZ | view,
            )
        })
        .any(|value| value.to_lowercase().contains("mactype"))
}

#[cfg(not(windows))]
fn registry_mode_detected() -> bool {
    false
}

#[cfg(windows)]
fn service_status() -> (bool, bool) {
    use windows_sys::Win32::System::Services::{
        CloseServiceHandle, OpenSCManagerW, OpenServiceW, QueryServiceStatusEx, SC_MANAGER_CONNECT,
        SC_STATUS_PROCESS_INFO, SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_STATUS_PROCESS,
    };
    let manager = unsafe { OpenSCManagerW(std::ptr::null(), std::ptr::null(), SC_MANAGER_CONNECT) };
    if manager.is_null() {
        return (false, false);
    }
    let name = wide("MacType");
    let service = unsafe { OpenServiceW(manager, name.as_ptr(), SERVICE_QUERY_STATUS) };
    if service.is_null() {
        unsafe { CloseServiceHandle(manager) };
        return (false, false);
    }
    let mut status = SERVICE_STATUS_PROCESS::default();
    let mut needed = 0u32;
    let ok = unsafe {
        QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            (&mut status as *mut SERVICE_STATUS_PROCESS).cast(),
            std::mem::size_of::<SERVICE_STATUS_PROCESS>() as u32,
            &mut needed,
        )
    } != 0;
    unsafe {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
    }
    (true, ok && status.dwCurrentState == SERVICE_RUNNING)
}

#[cfg(not(windows))]
fn service_status() -> (bool, bool) {
    (false, false)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manual_launcher_rejects_non_executable_targets() {
        let error = launch_with_mactype(Path::new("."), "Cargo.toml", &[]).unwrap_err();
        assert!(error.contains("existing .exe") || error.contains("cannot find"));
    }

    #[test]
    fn status_never_claims_unsupported_system_modes() {
        assert!(!status(None).system_modes_supported);
    }
}
