use super::super::RuntimeState;
use crate::machine_integration::scm_response::{ScmResponse, ScmResponseError};
use mactype_service_contract::SERVICE_NAME;
use std::{ffi::OsStr, mem::size_of, os::windows::ffi::OsStrExt};
use windows_sys::Win32::{
    Foundation::GetLastError,
    System::Services::{
        CloseServiceHandle, OpenSCManagerW, OpenServiceW, QueryServiceConfigW,
        QueryServiceStatusEx, QUERY_SERVICE_CONFIGW, SC_HANDLE, SC_MANAGER_CONNECT,
        SC_STATUS_PROCESS_INFO, SERVICE_PAUSED, SERVICE_QUERY_STATUS, SERVICE_RUNNING,
        SERVICE_START_PENDING, SERVICE_STATUS_PROCESS, SERVICE_STOPPED, SERVICE_STOP_PENDING,
    },
};

const MAX_DEPENDENCIES: usize = 256;

pub(super) struct ServiceHandle(pub(super) SC_HANDLE);

impl Drop for ServiceHandle {
    fn drop(&mut self) {
        unsafe { CloseServiceHandle(self.0) };
    }
}

pub(super) struct Configuration {
    pub(super) service_type: u32,
    pub(super) start_type: u32,
    pub(super) error_control: u32,
    pub(super) binary_path: String,
    pub(super) account: String,
    pub(super) display_name: String,
    pub(super) load_order_group: String,
    pub(super) tag_id: u32,
    pub(super) dependencies: Vec<String>,
}

pub(super) fn query_configuration(service: SC_HANDLE) -> Result<Configuration, u32> {
    let response = ScmResponse::query(
        size_of::<QUERY_SERVICE_CONFIGW>(),
        |buffer, capacity, needed| {
            // SAFETY: `service` is a live SCM handle; buffer and capacity
            // describe the response allocation or the documented null probe.
            unsafe { QueryServiceConfigW(service, buffer.cast(), capacity, needed) }
        },
    )
    .map_err(ScmResponseError::win32_code)?;
    let configuration = response
        .header::<QUERY_SERVICE_CONFIGW>()
        .map_err(ScmResponseError::win32_code)?;
    let dependencies = response
        .multi_units(configuration.lpDependencies, MAX_DEPENDENCIES)
        .map_err(ScmResponseError::win32_code)?
        .into_iter()
        .map(String::from_utf16_lossy)
        .collect();
    Ok(Configuration {
        service_type: configuration.dwServiceType,
        start_type: configuration.dwStartType,
        error_control: configuration.dwErrorControl,
        binary_path: response
            .wide_string_lossy(configuration.lpBinaryPathName)
            .map_err(ScmResponseError::win32_code)?,
        account: response
            .wide_string_lossy(configuration.lpServiceStartName)
            .map_err(ScmResponseError::win32_code)?,
        display_name: response
            .wide_string_lossy(configuration.lpDisplayName)
            .map_err(ScmResponseError::win32_code)?,
        load_order_group: response
            .wide_string_lossy(configuration.lpLoadOrderGroup)
            .map_err(ScmResponseError::win32_code)?,
        tag_id: configuration.dwTagId,
        dependencies,
    })
}

pub(super) fn query_runtime(service: SC_HANDLE) -> Result<(RuntimeState, u32), u32> {
    let mut status = SERVICE_STATUS_PROCESS::default();
    let mut needed = 0;
    if unsafe {
        QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            (&mut status as *mut SERVICE_STATUS_PROCESS).cast(),
            std::mem::size_of::<SERVICE_STATUS_PROCESS>() as u32,
            &mut needed,
        )
    } == 0
    {
        return Err(unsafe { GetLastError() });
    }
    let runtime = match status.dwCurrentState {
        SERVICE_STOPPED => RuntimeState::Stopped,
        SERVICE_START_PENDING => RuntimeState::StartPending,
        SERVICE_RUNNING => RuntimeState::Running,
        SERVICE_STOP_PENDING => RuntimeState::StopPending,
        SERVICE_PAUSED => RuntimeState::Paused,
        _ => RuntimeState::Unknown,
    };
    Ok((runtime, status.dwProcessId))
}

pub(in crate::machine_integration::open_service) fn running_service_process_id(
) -> Result<u32, String> {
    let manager = unsafe { OpenSCManagerW(std::ptr::null(), std::ptr::null(), SC_MANAGER_CONNECT) };
    if manager.is_null() {
        return Err(std::io::Error::last_os_error().to_string());
    }
    let manager = ServiceHandle(manager);
    let name = wide(SERVICE_NAME);
    let service = unsafe { OpenServiceW(manager.0, name.as_ptr(), SERVICE_QUERY_STATUS) };
    if service.is_null() {
        return Err(std::io::Error::last_os_error().to_string());
    }
    let service = ServiceHandle(service);
    let (runtime, process_id) = query_runtime(service.0)
        .map_err(|code| format!("QueryServiceStatusEx failed with {code}"))?;
    if runtime != RuntimeState::Running || process_id == 0 {
        return Err("the new service has no stable running SCM process".to_owned());
    }
    Ok(process_id)
}

pub(in crate::machine_integration::open_service) fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}
