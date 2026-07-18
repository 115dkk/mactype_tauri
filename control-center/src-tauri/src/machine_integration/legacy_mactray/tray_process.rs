use super::LegacyTrayProcessState;
use mactype_service_contract::StructuredServiceError;
use std::path::PathBuf;

#[cfg(windows)]
use std::{
    ffi::{OsStr, OsString},
    os::windows::ffi::{OsStrExt, OsStringExt},
    path::Path,
};

#[cfg(windows)]
use windows_sys::Win32::{
    Foundation::{CloseHandle, GetLastError, FILETIME, HANDLE},
    Storage::FileSystem::{
        GetFileAttributesW, FILE_ATTRIBUTE_REPARSE_POINT, INVALID_FILE_ATTRIBUTES, SYNCHRONIZE,
    },
    System::{
        RemoteDesktop::{
            ProcessIdToSessionId, WTSEnumerateProcessesW, WTSFreeMemory, WTS_PROCESS_INFOW,
        },
        Threading::{
            GetCurrentProcessId, GetProcessId, GetProcessTimes, OpenProcess,
            QueryFullProcessImageNameW, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION,
        },
    },
};

#[derive(Clone, Debug, PartialEq)]
pub(super) struct LegacyTrayProcessIdentity {
    pub(super) creation_time: u64,
    pub(super) session_id: u32,
    pub(super) path: PathBuf,
    pub(super) trusted_path: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub(super) struct LegacyTrayProcessObservation {
    pub(super) image_name: String,
    pub(super) pid: u32,
    pub(super) session_id: u32,
    pub(super) identity: Result<LegacyTrayProcessIdentity, StructuredServiceError>,
}

pub(crate) fn observe_tray_process() -> LegacyTrayProcessState {
    #[cfg(windows)]
    {
        observe_windows_tray_process()
    }
    #[cfg(not(windows))]
    {
        LegacyTrayProcessState::Absent
    }
}

#[cfg(windows)]
fn observe_windows_tray_process() -> LegacyTrayProcessState {
    let current_session_id = match process_session_id(unsafe { GetCurrentProcessId() }) {
        Ok(session_id) => session_id,
        Err(error) => return LegacyTrayProcessState::Unknown { error },
    };
    let entries = match enumerate_processes() {
        Ok(entries) => entries,
        Err(error) => return LegacyTrayProcessState::Unknown { error },
    };
    let observations = entries
        .into_iter()
        .filter(|entry| entry.image_name.eq_ignore_ascii_case("MacTray.exe"))
        .map(|entry| LegacyTrayProcessObservation {
            image_name: entry.image_name,
            pid: entry.pid,
            session_id: entry.session_id,
            identity: if entry.session_id == 0 {
                Err(service_error(
                    "legacy-tray-process-service-session",
                    "the session-zero MacTray process belongs to the legacy service",
                    None,
                ))
            } else {
                inspect_process(entry.pid)
            },
        })
        .collect();
    classify_tray_process_inventory(current_session_id, observations)
}

#[cfg(windows)]
struct EnumeratedProcess {
    image_name: String,
    pid: u32,
    session_id: u32,
}

#[cfg(windows)]
struct WtsProcessList(*mut WTS_PROCESS_INFOW);

#[cfg(windows)]
impl Drop for WtsProcessList {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { WTSFreeMemory(self.0.cast()) };
        }
    }
}

#[cfg(windows)]
fn enumerate_processes() -> Result<Vec<EnumeratedProcess>, StructuredServiceError> {
    let mut processes = std::ptr::null_mut();
    let mut count = 0_u32;
    if unsafe { WTSEnumerateProcessesW(std::ptr::null_mut(), 0, 1, &mut processes, &mut count) }
        == 0
    {
        return Err(last_error(
            "legacy-tray-process-enumeration-unavailable",
            "the running process inventory could not be enumerated",
        ));
    }
    let list = WtsProcessList(processes);
    if count == 0 {
        return Ok(Vec::new());
    }
    if list.0.is_null() {
        return Err(service_error(
            "legacy-tray-process-enumeration-invalid",
            "the running process inventory returned no process buffer",
            None,
        ));
    }
    let entries = unsafe { std::slice::from_raw_parts(list.0, count as usize) };
    entries
        .iter()
        .map(|entry| {
            Ok(EnumeratedProcess {
                image_name: process_name(entry.pProcessName)?,
                pid: entry.ProcessId,
                session_id: entry.SessionId,
            })
        })
        .collect()
}

#[cfg(windows)]
fn process_name(pointer: *const u16) -> Result<String, StructuredServiceError> {
    const MAX_PROCESS_NAME_UNITS: usize = 32_768;
    if pointer.is_null() {
        return Err(service_error(
            "legacy-tray-process-name-unavailable",
            "an enumerated process has no image name",
            None,
        ));
    }
    let mut length = 0;
    while length < MAX_PROCESS_NAME_UNITS && unsafe { *pointer.add(length) } != 0 {
        length += 1;
    }
    if length == MAX_PROCESS_NAME_UNITS {
        return Err(service_error(
            "legacy-tray-process-name-invalid",
            "an enumerated process image name is not bounded",
            None,
        ));
    }
    Ok(String::from_utf16_lossy(unsafe {
        std::slice::from_raw_parts(pointer, length)
    }))
}

#[cfg(windows)]
pub(super) struct ProcessHandle(HANDLE);

#[cfg(windows)]
impl ProcessHandle {
    pub(super) fn open(pid: u32) -> Result<Self, StructuredServiceError> {
        if pid == 0 {
            return Err(service_error(
                "legacy-tray-process-pid-invalid",
                "the MacTray process ID is zero",
                None,
            ));
        }
        let handle =
            unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, 0, pid) };
        if handle.is_null() {
            Err(last_error(
                "legacy-tray-process-inaccessible",
                "the MacTray process could not be opened for identity verification",
            ))
        } else {
            Ok(Self(handle))
        }
    }

    pub(super) fn creation_time(&self) -> Result<u64, StructuredServiceError> {
        let mut creation = FILETIME::default();
        let mut exit = FILETIME::default();
        let mut kernel = FILETIME::default();
        let mut user = FILETIME::default();
        if unsafe { GetProcessTimes(self.0, &mut creation, &mut exit, &mut kernel, &mut user) } == 0
        {
            return Err(last_error(
                "legacy-tray-process-creation-time-unavailable",
                "the MacTray process creation time could not be read",
            ));
        }
        let value = (u64::from(creation.dwHighDateTime) << 32) | u64::from(creation.dwLowDateTime);
        if value == 0 {
            return Err(service_error(
                "legacy-tray-process-creation-time-invalid",
                "the MacTray process creation time is zero",
                None,
            ));
        }
        Ok(value)
    }

    pub(super) fn image_path(&self) -> Result<PathBuf, StructuredServiceError> {
        let mut buffer = vec![0_u16; 32_768];
        let mut length = buffer.len() as u32;
        if unsafe {
            QueryFullProcessImageNameW(self.0, PROCESS_NAME_WIN32, buffer.as_mut_ptr(), &mut length)
        } == 0
        {
            return Err(last_error(
                "legacy-tray-process-path-unavailable",
                "the MacTray process image path could not be read",
            ));
        }
        if length == 0 || length as usize >= buffer.len() {
            return Err(service_error(
                "legacy-tray-process-path-invalid",
                "the MacTray process image path is invalid",
                None,
            ));
        }
        Ok(PathBuf::from(OsString::from_wide(
            &buffer[..length as usize],
        )))
    }

    pub(super) fn raw(&self) -> HANDLE {
        self.0
    }
}

#[cfg(windows)]
impl Drop for ProcessHandle {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.0) };
    }
}

#[cfg(windows)]
fn inspect_process(pid: u32) -> Result<LegacyTrayProcessIdentity, StructuredServiceError> {
    let process = ProcessHandle::open(pid)?;
    let actual_pid = unsafe { GetProcessId(process.0) };
    if actual_pid == 0 {
        return Err(last_error(
            "legacy-tray-process-pid-unavailable",
            "the opened MacTray process ID could not be read",
        ));
    }
    if actual_pid != pid {
        return Err(service_error(
            "legacy-tray-process-pid-changed",
            "the opened MacTray process ID does not match the observed process",
            None,
        ));
    }
    let creation_time = process.creation_time()?;
    let session_id = process_session_id(actual_pid)?;
    let path = process.image_path()?;
    Ok(LegacyTrayProcessIdentity {
        creation_time,
        session_id,
        trusted_path: trusted_mactray_process_path(&path),
        path,
    })
}

#[cfg(windows)]
pub(super) fn process_session_id(pid: u32) -> Result<u32, StructuredServiceError> {
    let mut session_id = 0;
    if unsafe { ProcessIdToSessionId(pid, &mut session_id) } == 0 {
        Err(last_error(
            "legacy-tray-process-session-unavailable",
            "the process session could not be read",
        ))
    } else {
        Ok(session_id)
    }
}

#[cfg(windows)]
pub(super) fn trusted_mactray_process_path(path: &Path) -> bool {
    let Some(expected) = super::windows::expected_mactray_path() else {
        return false;
    };
    if path_has_reparse_component(path) || path_has_reparse_component(&expected) {
        return false;
    }
    let Ok(actual) = std::fs::canonicalize(path) else {
        return false;
    };
    let Ok(expected) = std::fs::canonicalize(expected) else {
        return false;
    };
    same_windows_path(&actual, &expected)
}

#[cfg(windows)]
fn path_has_reparse_component(path: &Path) -> bool {
    path.ancestors().any(|component| {
        let wide = wide(component.as_os_str());
        let attributes = unsafe { GetFileAttributesW(wide.as_ptr()) };
        attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
    })
}

#[cfg(windows)]
pub(super) fn same_windows_path(left: &Path, right: &Path) -> bool {
    normalize_windows_path(left).eq_ignore_ascii_case(&normalize_windows_path(right))
}

#[cfg(windows)]
fn normalize_windows_path(path: &Path) -> String {
    let value = path.to_string_lossy();
    value
        .strip_prefix(r"\\?\")
        .unwrap_or(value.as_ref())
        .replace('/', "\\")
}

#[cfg(windows)]
fn wide(value: &OsStr) -> Vec<u16> {
    value.encode_wide().chain(Some(0)).collect()
}

#[cfg(windows)]
fn last_error(code: &str, message: &str) -> StructuredServiceError {
    service_error(code, message, Some(unsafe { GetLastError() }))
}

#[cfg(windows)]
fn service_error(code: &str, message: &str, win32_error: Option<u32>) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error,
    }
}

pub(super) fn classify_tray_process_inventory(
    current_session_id: u32,
    observations: Vec<LegacyTrayProcessObservation>,
) -> LegacyTrayProcessState {
    let mut candidates = observations
        .into_iter()
        .filter(|entry| {
            entry.image_name.eq_ignore_ascii_case("MacTray.exe") && entry.session_id != 0
        })
        .collect::<Vec<_>>();
    if candidates.is_empty() {
        return LegacyTrayProcessState::Absent;
    }
    if candidates.len() != 1 {
        return unknown(
            "legacy-tray-process-multiple",
            "multiple interactive MacTray processes were observed",
            None,
        );
    }
    let Some(entry) = candidates.pop() else {
        return LegacyTrayProcessState::Absent;
    };
    let identity = match entry.identity {
        Ok(identity) => identity,
        Err(error) => return LegacyTrayProcessState::Unknown { error },
    };
    if entry.pid == 0 || identity.creation_time == 0 || identity.session_id != entry.session_id {
        return unknown(
            "legacy-tray-process-identity-changed",
            "the MacTray process identity changed during inspection",
            None,
        );
    }
    if !identity.trusted_path {
        return LegacyTrayProcessState::UntrustedSameName {
            session_id: Some(identity.session_id),
            path: Some(identity.path),
        };
    }
    if identity.session_id == current_session_id {
        LegacyTrayProcessState::TrustedCurrentSession {
            pid: entry.pid,
            creation_time: identity.creation_time,
            path: identity.path,
        }
    } else {
        LegacyTrayProcessState::TrustedOtherSession {
            session_id: identity.session_id,
            path: identity.path,
        }
    }
}

fn unknown(code: &str, message: &str, win32_error: Option<u32>) -> LegacyTrayProcessState {
    LegacyTrayProcessState::Unknown {
        error: StructuredServiceError {
            code: code.to_owned(),
            message: message.to_owned(),
            win32_error,
        },
    }
}
