use super::super::*;
use crate::machine_integration::scm_response::{ScmResponse, ScmResponseError};
use std::{ffi::OsStr, mem::size_of, os::windows::ffi::OsStrExt, path::PathBuf};
use windows_sys::Win32::{
    Foundation::GetLastError,
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

/// Reads a NUL-terminated string with a fixed upper bound.
///
/// # Safety
///
/// `pointer` must be null or point to at least 32,768 readable `u16` values,
/// unless a NUL occurs earlier.
unsafe fn wide_string(pointer: *const u16) -> Option<String> {
    if pointer.is_null() {
        return None;
    }
    const MAX_UNITS: usize = 32_768;
    let mut length = 0;
    while length < MAX_UNITS {
        // SAFETY: the caller guarantees that values through `MAX_UNITS` are
        // readable unless an earlier NUL terminates the string.
        if unsafe { *pointer.add(length) } == 0 {
            // SAFETY: the scan proved that all `length` units are readable.
            let units = unsafe { std::slice::from_raw_parts(pointer, length) };
            return Some(String::from_utf16_lossy(units));
        }
        length += 1;
    }
    None
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
    // SAFETY: SHGetKnownFolderPath returned a CoTaskMem-allocated,
    // NUL-terminated path string on success.
    let root = unsafe { wide_string(pointer) };
    unsafe { CoTaskMemFree(pointer.cast()) };
    let root = std::fs::canonicalize(root?).ok()?;
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
    const MAX_DEPENDENCIES: usize = 256;

    let response = ScmResponse::query(
        size_of::<QUERY_SERVICE_CONFIGW>(),
        |buffer, capacity, needed| {
            // SAFETY: `service` holds a live SCM handle; buffer and capacity
            // describe the response allocation or the documented null probe.
            unsafe { QueryServiceConfigW(service.0, buffer.cast(), capacity, needed) }
        },
    )
    .map_err(ScmResponseError::win32_code)?;
    let raw = response
        .header::<QUERY_SERVICE_CONFIGW>()
        .map_err(ScmResponseError::win32_code)?;
    let load_order_group = response
        .wide_string_lossy(raw.lpLoadOrderGroup)
        .map_err(ScmResponseError::win32_code)?;
    let dependencies = response
        .multi_units(raw.lpDependencies, MAX_DEPENDENCIES)
        .map_err(ScmResponseError::win32_code)?
        .into_iter()
        .map(String::from_utf16_lossy)
        .collect();
    Ok(ServiceConfiguration {
        display_name: response
            .wide_string_lossy(raw.lpDisplayName)
            .map_err(ScmResponseError::win32_code)?,
        binary_path: response
            .wide_string_lossy(raw.lpBinaryPathName)
            .map_err(ScmResponseError::win32_code)?,
        service_type: raw.dwServiceType,
        start_type: raw.dwStartType,
        error_control: raw.dwErrorControl,
        load_order_group: (!load_order_group.is_empty()).then_some(load_order_group),
        tag_id: raw.dwTagId,
        account: response
            .wide_string_lossy(raw.lpServiceStartName)
            .map_err(ScmResponseError::win32_code)?,
        dependencies,
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
