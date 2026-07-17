use std::mem::size_of;
use std::ptr;

use mactype_service_contract::StructuredServiceError;
use windows_sys::Win32::Foundation::{
    GetLastError, ERROR_INSUFFICIENT_BUFFER, ERROR_SERVICE_DOES_NOT_EXIST,
};
use windows_sys::Win32::System::Services::{
    CloseServiceHandle, OpenSCManagerW, OpenServiceW, QueryServiceConfigW, QueryServiceStatusEx,
    QUERY_SERVICE_CONFIGW, SC_HANDLE, SC_MANAGER_CONNECT, SC_STATUS_PROCESS_INFO,
    SERVICE_QUERY_CONFIG, SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_START_PENDING,
    SERVICE_STATUS_PROCESS, SERVICE_STOPPED,
};

use crate::LegacyServiceRuntimeState;

const LEGACY_SERVICE_NAME: &str = "MacType";

pub(super) struct ServiceManager(SC_HANDLE);

impl ServiceManager {
    pub(super) fn open() -> Result<Self, StructuredServiceError> {
        let handle = unsafe { OpenSCManagerW(ptr::null(), ptr::null(), SC_MANAGER_CONNECT) };
        if handle.is_null() {
            return Err(last_error(
                "scm-inspection-unavailable",
                "the Service Control Manager could not be opened for inspection",
            ));
        }
        Ok(Self(handle))
    }

    pub(super) fn service_image(&self, name: &str) -> Result<String, StructuredServiceError> {
        let service = self.open_service(name, SERVICE_QUERY_CONFIG)?;
        let mut required = 0;
        unsafe { QueryServiceConfigW(service.0, ptr::null_mut(), 0, &mut required) };
        if unsafe { GetLastError() } != ERROR_INSUFFICIENT_BUFFER || required == 0 {
            return Err(last_error(
                "open-service-config-unavailable",
                "the open service ImagePath size could not be queried",
            ));
        }
        let word_count = (required as usize).div_ceil(size_of::<usize>());
        let mut buffer = vec![0usize; word_count];
        if unsafe {
            QueryServiceConfigW(
                service.0,
                buffer.as_mut_ptr().cast(),
                required,
                &mut required,
            )
        } == 0
        {
            return Err(last_error(
                "open-service-config-unavailable",
                "the open service ImagePath could not be read",
            ));
        }
        let config = unsafe { &*buffer.as_ptr().cast::<QUERY_SERVICE_CONFIGW>() };
        wide_pointer(config.lpBinaryPathName).ok_or_else(|| {
            service_error(
                "open-service-config-invalid",
                "the open service ImagePath is empty or invalid",
                None,
            )
        })
    }

    pub(super) fn legacy_state(&self) -> Result<LegacyServiceRuntimeState, StructuredServiceError> {
        let name = wide_null(LEGACY_SERVICE_NAME);
        let service = unsafe { OpenServiceW(self.0, name.as_ptr(), SERVICE_QUERY_STATUS) };
        if service.is_null() {
            let error = unsafe { GetLastError() };
            if error == ERROR_SERVICE_DOES_NOT_EXIST {
                return Ok(LegacyServiceRuntimeState::Absent);
            }
            return Err(service_error(
                "legacy-service-inspection-failed",
                "the legacy service state could not be inspected",
                Some(error as i32),
            ));
        }
        let service = ServiceHandle(service);
        let mut status = SERVICE_STATUS_PROCESS::default();
        let mut required = 0;
        if unsafe {
            QueryServiceStatusEx(
                service.0,
                SC_STATUS_PROCESS_INFO,
                (&mut status as *mut SERVICE_STATUS_PROCESS).cast::<u8>(),
                size_of::<SERVICE_STATUS_PROCESS>() as u32,
                &mut required,
            )
        } == 0
        {
            return Err(last_error(
                "legacy-service-inspection-failed",
                "the legacy service state could not be read",
            ));
        }
        Ok(match status.dwCurrentState {
            SERVICE_STOPPED => LegacyServiceRuntimeState::Stopped,
            SERVICE_START_PENDING => LegacyServiceRuntimeState::StartPending,
            SERVICE_RUNNING => LegacyServiceRuntimeState::Running,
            _ => LegacyServiceRuntimeState::Unknown,
        })
    }

    fn open_service(
        &self,
        name: &str,
        access: u32,
    ) -> Result<ServiceHandle, StructuredServiceError> {
        let name = wide_null(name);
        let handle = unsafe { OpenServiceW(self.0, name.as_ptr(), access) };
        if handle.is_null() {
            return Err(last_error(
                "open-service-config-unavailable",
                "the fixed open service registration could not be opened",
            ));
        }
        Ok(ServiceHandle(handle))
    }
}

impl Drop for ServiceManager {
    fn drop(&mut self) {
        unsafe { CloseServiceHandle(self.0) };
    }
}

struct ServiceHandle(SC_HANDLE);

impl Drop for ServiceHandle {
    fn drop(&mut self) {
        unsafe { CloseServiceHandle(self.0) };
    }
}

fn wide_pointer(pointer: *const u16) -> Option<String> {
    if pointer.is_null() {
        return None;
    }
    let mut length = 0usize;
    while length < 32_768 && unsafe { *pointer.add(length) } != 0 {
        length += 1;
    }
    if length == 0 || length == 32_768 {
        return None;
    }
    Some(String::from_utf16_lossy(unsafe {
        std::slice::from_raw_parts(pointer, length)
    }))
}

fn wide_null(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}

fn last_error(code: &str, message: &str) -> StructuredServiceError {
    service_error(code, message, Some(unsafe { GetLastError() } as i32))
}

fn service_error(code: &str, message: &str, win32_error: Option<i32>) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error: win32_error.map(|code| code as u32),
    }
}
