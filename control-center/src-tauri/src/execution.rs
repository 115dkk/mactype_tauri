use crate::{installation_root, profile::ProfileState};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    env, fs,
    io::Write,
    path::{Path, PathBuf},
    process::Command,
    thread,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tauri::State;

const RUNTIME_ARTIFACTS: [(&str, bool); 4] = [
    ("MacLoader.exe", true),
    ("MacType.dll", true),
    ("MacLoader64.exe", false),
    ("MacType64.dll", false),
];

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct ActiveRuntime {
    runtime_root: PathBuf,
    source_profile: PathBuf,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionTarget {
    pub target: String,
    pub arguments: Vec<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppliedProfile {
    pub source_profile: String,
    pub runtime_root: String,
}

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
    pub injection_ready: bool,
    pub active_profile: Option<String>,
    pub session_targets: Vec<SessionTarget>,
}

fn data_root() -> Result<PathBuf, String> {
    env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .map(|path| path.join("MacType").join("ControlCenter"))
        .ok_or_else(|| "LOCALAPPDATA is not available".to_owned())
}

fn runtime_root() -> Result<PathBuf, String> {
    Ok(data_root()?.join("runtime"))
}

fn atomic_write(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = path
        .parent()
        .ok_or_else(|| "destination has no parent directory".to_owned())?;
    fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| error.to_string())?
        .as_nanos();
    let temporary = path.with_extension(format!("tmp-{}-{nonce}", std::process::id()));
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)
        .map_err(|error| error.to_string())?;
    file.write_all(bytes).map_err(|error| error.to_string())?;
    file.sync_all().map_err(|error| error.to_string())?;
    drop(file);
    replace_file(&temporary, path).inspect_err(|_| {
        let _ = fs::remove_file(&temporary);
    })
}

#[cfg(windows)]
fn replace_file(source: &Path, destination: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{
        MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
    };

    let source = source
        .as_os_str()
        .encode_wide()
        .chain(Some(0))
        .collect::<Vec<_>>();
    let destination = destination
        .as_os_str()
        .encode_wide()
        .chain(Some(0))
        .collect::<Vec<_>>();
    let result = unsafe {
        MoveFileExW(
            source.as_ptr(),
            destination.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if result == 0 {
        Err(std::io::Error::last_os_error().to_string())
    } else {
        Ok(())
    }
}

#[cfg(not(windows))]
fn replace_file(source: &Path, destination: &Path) -> Result<(), String> {
    if destination.exists() {
        fs::remove_file(destination).map_err(|error| error.to_string())?;
    }
    fs::rename(source, destination).map_err(|error| error.to_string())
}

fn active_runtime_from(base: &Path) -> Result<ActiveRuntime, String> {
    let bytes = fs::read(base.join("active.json")).map_err(|error| error.to_string())?;
    let active: ActiveRuntime =
        serde_json::from_slice(&bytes).map_err(|error| error.to_string())?;
    let generations =
        fs::canonicalize(base.join("generations")).map_err(|error| error.to_string())?;
    let root = fs::canonicalize(&active.runtime_root).map_err(|error| error.to_string())?;
    if !root.starts_with(&generations)
        || !root.join("MacLoader.exe").is_file()
        || !root.join("MacType.dll").is_file()
        || !root.join("MacType.ini").is_file()
        || !root.join("profile.ini").is_file()
    {
        return Err(
            "active MacType runtime is incomplete or outside the managed directory".to_owned(),
        );
    }
    Ok(ActiveRuntime {
        runtime_root: root,
        ..active
    })
}

fn active_runtime() -> Result<ActiveRuntime, String> {
    active_runtime_from(&runtime_root()?)
}

fn prepare_runtime_at(
    base: &Path,
    installation_root: &Path,
    source_profile: &Path,
    profile_bytes: &[u8],
) -> Result<ActiveRuntime, String> {
    let installation = fs::canonicalize(installation_root).map_err(|error| error.to_string())?;
    let mut sources = Vec::new();
    let mut fingerprint = Sha256::new();
    fingerprint.update(profile_bytes);
    for (name, required) in RUNTIME_ARTIFACTS {
        let candidate = installation.join(name);
        if !candidate.is_file() {
            if required {
                return Err(format!("{name} was not found in the selected installation"));
            }
            continue;
        }
        let source = fs::canonicalize(&candidate).map_err(|error| error.to_string())?;
        if source.parent() != Some(installation.as_path()) {
            return Err(format!("{name} resolves outside the selected installation"));
        }
        let bytes = fs::read(&source).map_err(|error| error.to_string())?;
        fingerprint.update(name.as_bytes());
        fingerprint.update(&bytes);
        sources.push((name, source));
    }
    let digest = fingerprint.finalize();
    let id = digest[..12]
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<String>();
    let generations = base.join("generations");
    fs::create_dir_all(&generations).map_err(|error| error.to_string())?;
    let generation = generations.join(&id);
    if !generation.exists() {
        let staging = generations.join(format!(".stage-{id}-{}", std::process::id()));
        if staging.exists() {
            fs::remove_dir_all(&staging).map_err(|error| error.to_string())?;
        }
        fs::create_dir(&staging).map_err(|error| error.to_string())?;
        for (name, source) in &sources {
            fs::copy(source, staging.join(name)).map_err(|error| error.to_string())?;
        }
        fs::write(staging.join("profile.ini"), profile_bytes).map_err(|error| error.to_string())?;
        fs::write(
            staging.join("MacType.ini"),
            b"[General]\r\nAlternativeFile=profile.ini\r\n",
        )
        .map_err(|error| error.to_string())?;
        match fs::rename(&staging, &generation) {
            Ok(()) => {}
            Err(_) if generation.is_dir() => {
                fs::remove_dir_all(&staging).map_err(|error| error.to_string())?;
            }
            Err(error) => return Err(error.to_string()),
        }
    }
    let active = ActiveRuntime {
        runtime_root: generation,
        source_profile: source_profile.to_path_buf(),
    };
    atomic_write(
        &base.join("active.json"),
        &serde_json::to_vec_pretty(&active).map_err(|error| error.to_string())?,
    )?;
    active_runtime_from(base)
}

pub fn apply_profile(
    installation_root: &Path,
    source_profile: &Path,
    profile_bytes: &[u8],
) -> Result<AppliedProfile, String> {
    let active = prepare_runtime_at(
        &runtime_root()?,
        installation_root,
        source_profile,
        profile_bytes,
    )?;
    Ok(AppliedProfile {
        source_profile: active.source_profile.to_string_lossy().into_owned(),
        runtime_root: active.runtime_root.to_string_lossy().into_owned(),
    })
}

pub fn status(installation_root: Option<&Path>) -> ExecutionStatus {
    let (legacy_service_detected, legacy_service_running) = service_status();
    let active = active_runtime().ok();
    ExecutionStatus {
        tray_available: true,
        auto_start: autostart_value().is_some(),
        manual_launcher_available: installation_root.is_some() && active.is_some(),
        legacy_service_detected,
        legacy_service_running,
        registry_mode_detected: registry_mode_detected(),
        system_modes_supported: false,
        system_mode_note: "기존 MacTray 서비스는 비공개 Delphi 실행 파일이므로 제어하지 않습니다. AppInit 레지스트리 모드는 공식 프로젝트가 부팅 장애 위험 때문에 제거했으므로 읽기 전용으로만 감지합니다.".to_owned(),
        injection_ready: active.is_some(),
        active_profile: active.map(|runtime| runtime.source_profile.to_string_lossy().into_owned()),
        session_targets: session_targets().unwrap_or_default(),
    }
}

pub fn set_autostart(enabled: bool) -> Result<bool, String> {
    set_autostart_impl(enabled)?;
    Ok(autostart_value().is_some())
}

fn validate_launch(target: &str, arguments: &[String]) -> Result<PathBuf, String> {
    if arguments.len() > 32 || arguments.iter().any(|argument| argument.len() > 4096) {
        return Err(
            "manual launch accepts at most 32 arguments of 4096 characters each".to_owned(),
        );
    }
    let target = fs::canonicalize(target).map_err(|error| error.to_string())?;
    if !target.is_file()
        || !target
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("exe"))
    {
        return Err("manual launch target must be an existing .exe file".to_owned());
    }
    Ok(target)
}

fn launch_with_mactype_impl(target: &str, arguments: &[String]) -> Result<u32, String> {
    let target = validate_launch(target, arguments)?;
    let active =
        active_runtime().map_err(|_| "apply a profile before launching with MacType".to_owned())?;
    let loader = active.runtime_root.join("MacLoader.exe");
    Command::new(loader)
        .arg(&target)
        .args(arguments)
        .current_dir(&active.runtime_root)
        .spawn()
        .map(|child| child.id())
        .map_err(|error| error.to_string())
}

fn session_targets_path() -> Result<PathBuf, String> {
    Ok(data_root()?.join("session-targets.json"))
}

pub fn session_targets() -> Result<Vec<SessionTarget>, String> {
    let path = session_targets_path()?;
    if !path.exists() {
        return Ok(Vec::new());
    }
    let bytes = fs::read(path).map_err(|error| error.to_string())?;
    let targets: Vec<SessionTarget> =
        serde_json::from_slice(&bytes).map_err(|error| error.to_string())?;
    if targets.len() > 32 {
        return Err("session target list exceeds 32 entries".to_owned());
    }
    Ok(targets)
}

fn register_session_target_impl(
    target: &str,
    arguments: &[String],
) -> Result<Vec<SessionTarget>, String> {
    let target = validate_launch(target, arguments)?
        .to_string_lossy()
        .into_owned();
    let mut targets = session_targets()?;
    if !targets
        .iter()
        .any(|entry| entry.target.eq_ignore_ascii_case(&target))
    {
        if targets.len() == 32 {
            return Err("session target list already contains 32 entries".to_owned());
        }
        targets.push(SessionTarget {
            target,
            arguments: arguments.to_vec(),
        });
    }
    atomic_write(
        &session_targets_path()?,
        &serde_json::to_vec_pretty(&targets).map_err(|error| error.to_string())?,
    )?;
    Ok(targets)
}

fn remove_session_target_impl(target: &str) -> Result<Vec<SessionTarget>, String> {
    let mut targets = session_targets()?;
    targets.retain(|entry| !entry.target.eq_ignore_ascii_case(target));
    atomic_write(
        &session_targets_path()?,
        &serde_json::to_vec_pretty(&targets).map_err(|error| error.to_string())?,
    )?;
    Ok(targets)
}

fn launch_registered_targets_impl() -> Result<Vec<u32>, String> {
    session_targets()?
        .iter()
        .map(|entry| launch_with_mactype_impl(&entry.target, &entry.arguments))
        .collect()
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

#[tauri::command]
pub(crate) fn execution_status() -> ExecutionStatus {
    status(installation_root().as_deref())
}

#[tauri::command]
pub(crate) fn set_session_autostart(enabled: bool) -> Result<bool, String> {
    set_autostart(enabled)
}

#[tauri::command]
pub(crate) fn launch_with_mactype(target: String, arguments: Vec<String>) -> Result<u32, String> {
    launch_with_mactype_impl(&target, &arguments)
}

#[tauri::command]
pub(crate) fn apply_open_profile(state: State<'_, ProfileState>) -> Result<AppliedProfile, String> {
    let root =
        installation_root().ok_or_else(|| "MacType installation was not found".to_owned())?;
    let guard = state
        .0
        .lock()
        .map_err(|_| "profile lock is poisoned".to_owned())?;
    let profile = guard
        .as_ref()
        .ok_or_else(|| "no profile is open".to_owned())?;
    apply_profile(&root, profile.path(), &profile.encoded()?)
}

#[tauri::command]
pub(crate) fn register_session_target(
    target: String,
    arguments: Vec<String>,
) -> Result<Vec<SessionTarget>, String> {
    register_session_target_impl(&target, &arguments)
}

#[tauri::command]
pub(crate) fn remove_session_target(target: String) -> Result<Vec<SessionTarget>, String> {
    remove_session_target_impl(&target)
}

#[tauri::command]
pub(crate) fn launch_registered_targets() -> Result<Vec<u32>, String> {
    launch_registered_targets_impl()
}

#[tauri::command]
pub(crate) fn ci_verify_injection_workflow() -> Result<(), String> {
    let smoke_marker = env::var_os("MACTYPE_CI_SMOKE_FILE").ok_or_else(|| {
        "injection verification is available only during CI smoke tests".to_owned()
    })?;
    let target = env::var_os("MACTYPE_CI_MANUAL_TARGET")
        .ok_or_else(|| "MACTYPE_CI_MANUAL_TARGET is not available".to_owned())?;
    let marker = PathBuf::from(smoke_marker)
        .parent()
        .ok_or_else(|| "CI marker has no parent directory".to_owned())?
        .join("injection.ready");
    if marker.exists() {
        fs::remove_file(&marker).map_err(|error| error.to_string())?;
    }
    let target = target.to_string_lossy().into_owned();
    let arguments = vec![marker.to_string_lossy().into_owned()];
    register_session_target_impl(&target, &arguments)?;
    launch_registered_targets_impl()?;
    let deadline = std::time::Instant::now() + Duration::from_secs(10);
    while !marker.is_file() && std::time::Instant::now() < deadline {
        thread::sleep(Duration::from_millis(100));
    }
    remove_session_target_impl(&target)?;
    if !marker.is_file() {
        return Err("managed MacLoader did not start the registered injected target".to_owned());
    }
    let content = fs::read_to_string(&marker).map_err(|error| error.to_string())?;
    fs::remove_file(&marker).map_err(|error| error.to_string())?;
    if content.trim() != "mactype-manual-launch-ready" {
        return Err(format!(
            "injected target wrote an invalid marker: {content}"
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manual_launcher_rejects_non_executable_targets() {
        let error = launch_with_mactype_impl("Cargo.toml", &[]).unwrap_err();
        assert!(error.contains("existing .exe") || error.contains("cannot find"));
    }

    #[test]
    fn status_never_claims_unsupported_system_modes() {
        assert!(!status(None).system_modes_supported);
    }

    #[test]
    fn applied_profile_builds_a_self_contained_loader_generation() {
        let root = env::temp_dir().join(format!("mactype-runtime-test-{}", std::process::id()));
        let installation = root.join("installation");
        let runtime = root.join("runtime");
        fs::create_dir_all(&installation).unwrap();
        fs::write(installation.join("MacLoader.exe"), b"loader").unwrap();
        fs::write(installation.join("MacType.dll"), b"core").unwrap();
        let profile = b"[General]\r\nNormalWeight=7\r\n";
        let active = prepare_runtime_at(
            &runtime,
            &installation,
            Path::new("C:/profiles/User.ini"),
            profile,
        )
        .unwrap();
        assert_eq!(
            fs::read(active.runtime_root.join("profile.ini")).unwrap(),
            profile
        );
        assert_eq!(
            fs::read(active.runtime_root.join("MacType.ini")).unwrap(),
            b"[General]\r\nAlternativeFile=profile.ini\r\n"
        );
        assert!(active.runtime_root.join("MacLoader.exe").is_file());
        assert!(active.runtime_root.join("MacType.dll").is_file());
        let reopened = active_runtime_from(&runtime).unwrap();
        assert_eq!(reopened.source_profile, Path::new("C:/profiles/User.ini"));
        fs::remove_dir_all(root).unwrap();
    }
}
