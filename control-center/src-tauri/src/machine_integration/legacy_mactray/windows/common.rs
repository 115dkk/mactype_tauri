use super::super::*;
use std::{ffi::OsStr, os::windows::ffi::OsStrExt, path::PathBuf};
use windows_sys::Win32::{
    Foundation::{GetLastError, ERROR_INSUFFICIENT_BUFFER},
    System::{
        Com::CoTaskMemFree,
        Services::{
            CloseServiceHandle, OpenSCManagerW, OpenServiceW, QueryServiceConfigW,
            QueryServiceStatusEx, QUERY_SERVICE_CONFIGW, SC_HANDLE, SC_MANAGER_CONNECT,
            SC_STATUS_PROCESS_INFO, SERVICE_CONTINUE_PENDING, SERVICE_PAUSED,
            SERVICE_PAUSE_PENDING, SERVICE_RUNNING, SERVICE_START_PENDING, SERVICE_STATUS_PROCESS,
            SERVICE_STOPPED, SERVICE_STOP_PENDING,
        },
    },
    UI::Shell::{FOLDERID_ProgramFiles, SHGetKnownFolderPath},
};

pub(super) struct ServiceHandle(pub(super) SC_HANDLE);

impl Drop for ServiceHandle {
    fn drop(&mut self) {
        unsafe { CloseServiceHandle(self.0) };
    }
}

pub(super) fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

pub(super) fn wide_multi(values: &[String]) -> Vec<u16> {
    values
        .iter()
        .flat_map(|value| value.encode_utf16().chain(Some(0)))
        .chain(Some(0))
        .collect()
}

pub(super) unsafe fn wide_string(pointer: *const u16) -> String {
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

fn program_files_root() -> Option<PathBuf> {
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
    root.is_dir().then_some(root)
}

pub(super) fn expected_mactray_path() -> Option<PathBuf> {
    program_files_root().map(|root| root.join("MacType").join("MacTray.exe"))
}

pub(super) fn trusted_mactray_path() -> Option<PathBuf> {
    let root = program_files_root()?;
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

pub(super) fn query_runtime(service: &ServiceHandle) -> Result<ServiceRuntimeState, u32> {
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
        Err(unsafe { GetLastError() })
    } else {
        Ok(runtime_state(process_status.dwCurrentState))
    }
}

pub(super) fn query_configuration(service: &ServiceHandle) -> Result<ServiceConfiguration, u32> {
    let mut needed = 0;
    let initial_query =
        unsafe { QueryServiceConfigW(service.0, std::ptr::null_mut(), 0, &mut needed) };
    let initial_error = unsafe { GetLastError() };
    if initial_query != 0
        || initial_error != ERROR_INSUFFICIENT_BUFFER
        || needed < std::mem::size_of::<QUERY_SERVICE_CONFIGW>() as u32
    {
        return Err(initial_error);
    }
    let word_size = std::mem::size_of::<usize>();
    let mut buffer = vec![0usize; (needed as usize).div_ceil(word_size)];
    let configuration = buffer.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>();
    if unsafe { QueryServiceConfigW(service.0, configuration, needed, &mut needed) } == 0 {
        return Err(unsafe { GetLastError() });
    }
    let raw = unsafe { &*configuration };
    let load_order_group = unsafe { wide_string(raw.lpLoadOrderGroup) };
    Ok(ServiceConfiguration {
        display_name: unsafe { wide_string(raw.lpDisplayName) },
        binary_path: unsafe { wide_string(raw.lpBinaryPathName) },
        service_type: raw.dwServiceType,
        start_type: raw.dwStartType,
        error_control: raw.dwErrorControl,
        load_order_group: (!load_order_group.is_empty()).then_some(load_order_group),
        tag_id: raw.dwTagId,
        account: unsafe { wide_string(raw.lpServiceStartName) },
        dependencies: unsafe { multi_string(raw.lpDependencies) },
    })
}

pub(super) fn open_for(access: u32) -> Result<ServiceHandle, u32> {
    let manager = unsafe { OpenSCManagerW(std::ptr::null(), std::ptr::null(), SC_MANAGER_CONNECT) };
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
