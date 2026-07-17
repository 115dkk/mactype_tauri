use super::super::RuntimeState;
use mactype_service_contract::SERVICE_NAME;
use std::{ffi::OsStr, os::windows::ffi::OsStrExt};
use windows_sys::Win32::{
    Foundation::{GetLastError, ERROR_INSUFFICIENT_BUFFER},
    System::Services::{
        CloseServiceHandle, OpenSCManagerW, OpenServiceW, QueryServiceConfigW,
        QueryServiceStatusEx, QUERY_SERVICE_CONFIGW, SC_HANDLE, SC_MANAGER_CONNECT,
        SC_STATUS_PROCESS_INFO, SERVICE_PAUSED, SERVICE_QUERY_STATUS, SERVICE_RUNNING,
        SERVICE_START_PENDING, SERVICE_STATUS_PROCESS, SERVICE_STOPPED, SERVICE_STOP_PENDING,
    },
};

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
    let mut required = 0;
    unsafe { QueryServiceConfigW(service, std::ptr::null_mut(), 0, &mut required) };
    let error = unsafe { GetLastError() };
    if error != ERROR_INSUFFICIENT_BUFFER || required == 0 {
        return Err(error);
    }
    let mut buffer = vec![0_u8; required as usize];
    let configuration = buffer.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>();
    if unsafe { QueryServiceConfigW(service, configuration, required, &mut required) } == 0 {
        return Err(unsafe { GetLastError() });
    }
    let configuration = unsafe { &*configuration };
    Ok(Configuration {
        service_type: configuration.dwServiceType,
        start_type: configuration.dwStartType,
        error_control: configuration.dwErrorControl,
        binary_path: unsafe { wide_pointer(configuration.lpBinaryPathName) },
        account: unsafe { wide_pointer(configuration.lpServiceStartName) },
        display_name: unsafe { wide_pointer(configuration.lpDisplayName) },
        load_order_group: unsafe { wide_pointer(configuration.lpLoadOrderGroup) },
        tag_id: configuration.dwTagId,
        dependencies: unsafe { wide_multi_pointer(configuration.lpDependencies) },
    })
}

unsafe fn wide_multi_pointer(value: *const u16) -> Vec<String> {
    if value.is_null() {
        return Vec::new();
    }
    let mut entries = Vec::new();
    let mut offset = 0usize;
    loop {
        let start = unsafe { value.add(offset) };
        if unsafe { *start } == 0 {
            break;
        }
        let mut length = 0usize;
        while unsafe { *start.add(length) } != 0 {
            length += 1;
        }
        entries.push(String::from_utf16_lossy(unsafe {
            std::slice::from_raw_parts(start, length)
        }));
        offset += length + 1;
    }
    entries
}

pub(super) fn query_runtime(service: SC_HANDLE) -> Result<(RuntimeState, u32), u32> {
    let mut status: SERVICE_STATUS_PROCESS = unsafe { std::mem::zeroed() };
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

unsafe fn wide_pointer(value: *const u16) -> String {
    if value.is_null() {
        return String::new();
    }
    let mut length = 0;
    while unsafe { *value.add(length) } != 0 {
        length += 1;
    }
    String::from_utf16_lossy(unsafe { std::slice::from_raw_parts(value, length) })
}

pub(in crate::machine_integration::open_service) fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}
