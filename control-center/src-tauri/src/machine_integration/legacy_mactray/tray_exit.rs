use super::LegacyTrayProcessState;
use serde::Deserialize;
use std::path::PathBuf;

#[derive(Clone, Debug, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct LegacyTrayExitRequest {
    pub(crate) pid: u32,
    #[serde(deserialize_with = "super::model::decimal_u64::deserialize")]
    pub(crate) creation_time: u64,
    pub(crate) path: PathBuf,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum LegacyTrayExitOutcome {
    Exited,
    TimedOut,
    ProtocolUnavailable,
}

pub(super) trait LegacyTrayExitBackend {
    fn observe_process(&mut self) -> LegacyTrayProcessState;
    fn request_official_exit(
        &mut self,
        expected: &LegacyTrayExitRequest,
    ) -> Result<LegacyTrayExitOutcome, String>;
}

pub(super) fn request_tray_exit_with(
    backend: &mut impl LegacyTrayExitBackend,
    expected: &LegacyTrayExitRequest,
) -> Result<(), String> {
    require_exact_identity(backend.observe_process(), expected)?;
    require_exact_identity(backend.observe_process(), expected)?;
    match backend.request_official_exit(expected)? {
        LegacyTrayExitOutcome::Exited => {}
        LegacyTrayExitOutcome::TimedOut => {
            return Err("the graceful MacTray exit request timed out".to_owned());
        }
        LegacyTrayExitOutcome::ProtocolUnavailable => {
            return Err("the official MacTray exit protocol is unavailable".to_owned());
        }
    }
    if backend.observe_process() != LegacyTrayProcessState::Absent {
        return Err("MacTray remained present after the graceful exit request".to_owned());
    }
    Ok(())
}

fn require_exact_identity(
    observed: LegacyTrayProcessState,
    expected: &LegacyTrayExitRequest,
) -> Result<(), String> {
    match observed {
        LegacyTrayProcessState::TrustedCurrentSession {
            pid,
            creation_time,
            path,
        } if pid == expected.pid
            && creation_time == expected.creation_time
            && paths_match(&path, &expected.path) =>
        {
            Ok(())
        }
        _ => Err("the observed MacTray process identity changed before graceful exit".to_owned()),
    }
}

#[cfg(windows)]
fn paths_match(left: &std::path::Path, right: &std::path::Path) -> bool {
    super::tray_process::same_windows_path(left, right)
}

#[cfg(not(windows))]
fn paths_match(left: &std::path::Path, right: &std::path::Path) -> bool {
    left == right
}

struct SystemTrayExitBackend;

impl LegacyTrayExitBackend for SystemTrayExitBackend {
    fn observe_process(&mut self) -> LegacyTrayProcessState {
        super::observe_tray_process()
    }

    fn request_official_exit(
        &mut self,
        expected: &LegacyTrayExitRequest,
    ) -> Result<LegacyTrayExitOutcome, String> {
        request_official_exit(expected)
    }
}

pub(crate) fn request_tray_exit(expected: &LegacyTrayExitRequest) -> Result<(), String> {
    request_tray_exit_with(&mut SystemTrayExitBackend, expected)
}

pub(super) fn official_exit_available(process: &LegacyTrayProcessState) -> bool {
    let LegacyTrayProcessState::TrustedCurrentSession { pid, .. } = process else {
        return false;
    };
    owned_windows(*pid).is_ok_and(|windows| !windows.is_empty())
}

#[cfg(not(windows))]
fn request_official_exit(
    _expected: &LegacyTrayExitRequest,
) -> Result<LegacyTrayExitOutcome, String> {
    Ok(LegacyTrayExitOutcome::ProtocolUnavailable)
}

#[cfg(not(windows))]
fn owned_windows(_pid: u32) -> Result<Vec<isize>, String> {
    Ok(Vec::new())
}

#[cfg(windows)]
fn request_official_exit(
    expected: &LegacyTrayExitRequest,
) -> Result<LegacyTrayExitOutcome, String> {
    use super::tray_process::{process_session_id, trusted_mactray_process_path, ProcessHandle};
    use windows_sys::Win32::{
        Foundation::{GetLastError, WAIT_FAILED, WAIT_OBJECT_0, WAIT_TIMEOUT},
        System::Threading::{GetCurrentProcessId, GetProcessId, WaitForSingleObject},
        UI::WindowsAndMessaging::{RegisterWindowMessageW, SendNotifyMessageW},
    };

    const EXIT_WAIT_MS: u32 = 5_000;
    let process = ProcessHandle::open(expected.pid).map_err(format_service_error)?;
    let actual_pid = unsafe { GetProcessId(process.raw()) };
    if actual_pid != expected.pid
        || process.creation_time().map_err(format_service_error)? != expected.creation_time
        || process_session_id(actual_pid).map_err(format_service_error)?
            != process_session_id(unsafe { GetCurrentProcessId() }).map_err(format_service_error)?
    {
        return Err("the opened MacTray process identity changed before graceful exit".to_owned());
    }
    let path = process.image_path().map_err(format_service_error)?;
    if !trusted_mactray_process_path(&path) || !paths_match(&path, &expected.path) {
        return Err("the opened MacTray process path is not the trusted installation".to_owned());
    }

    let windows = owned_windows(expected.pid)?;
    if windows.is_empty() {
        return Ok(LegacyTrayExitOutcome::ProtocolUnavailable);
    }
    let message_name: Vec<u16> = "MacType_Exit_Notify\0".encode_utf16().collect();
    let message = unsafe { RegisterWindowMessageW(message_name.as_ptr()) };
    if message == 0 {
        return Err(format!("RegisterWindowMessageW failed with {}", unsafe {
            GetLastError()
        }));
    }
    for window in windows {
        if unsafe { SendNotifyMessageW(window, message, 0, 0) } == 0 {
            return Err(format!("SendNotifyMessageW failed with {}", unsafe {
                GetLastError()
            }));
        }
    }
    match unsafe { WaitForSingleObject(process.raw(), EXIT_WAIT_MS) } {
        WAIT_OBJECT_0 => Ok(LegacyTrayExitOutcome::Exited),
        WAIT_TIMEOUT => Ok(LegacyTrayExitOutcome::TimedOut),
        WAIT_FAILED => Err(format!("waiting for MacTray exit failed with {}", unsafe {
            GetLastError()
        })),
        other => Err(format!("waiting for MacTray exit returned {other}")),
    }
}

#[cfg(windows)]
fn owned_windows(pid: u32) -> Result<Vec<windows_sys::Win32::Foundation::HWND>, String> {
    use windows_sys::Win32::{
        Foundation::{HWND, LPARAM},
        UI::WindowsAndMessaging::{EnumWindows, GetWindowThreadProcessId},
    };

    struct Context {
        pid: u32,
        windows: Vec<HWND>,
    }

    unsafe extern "system" fn collect(window: HWND, parameter: LPARAM) -> i32 {
        let context = unsafe { &mut *(parameter as *mut Context) };
        let mut owner_pid = 0;
        unsafe { GetWindowThreadProcessId(window, &mut owner_pid) };
        if owner_pid == context.pid {
            context.windows.push(window);
        }
        1
    }

    let mut context = Context {
        pid,
        windows: Vec::new(),
    };
    if unsafe { EnumWindows(Some(collect), (&mut context as *mut Context) as LPARAM) } == 0 {
        return Err("the MacTray window inventory could not be enumerated".to_owned());
    }
    Ok(context.windows)
}

#[cfg(windows)]
fn format_service_error(error: mactype_service_contract::StructuredServiceError) -> String {
    match error.win32_error {
        Some(win32) => format!("{}: {} ({win32})", error.code, error.message),
        None => format!("{}: {}", error.code, error.message),
    }
}
