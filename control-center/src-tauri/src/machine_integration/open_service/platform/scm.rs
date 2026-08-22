use super::super::RuntimeState;
use mactype_service_contract::SERVICE_NAME;
use std::{
    ffi::OsStr,
    mem::{align_of, size_of},
    os::windows::ffi::OsStrExt,
};
use windows_sys::Win32::{
    Foundation::{GetLastError, ERROR_INSUFFICIENT_BUFFER, ERROR_INVALID_DATA},
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
    let words = (required as usize).div_ceil(size_of::<usize>());
    let mut buffer = vec![0_usize; words];
    let capacity_bytes = std::mem::size_of_val(buffer.as_slice());
    if capacity_bytes < size_of::<QUERY_SERVICE_CONFIGW>() {
        return Err(ERROR_INVALID_DATA);
    }
    let capacity = u32::try_from(capacity_bytes).map_err(|_| ERROR_INVALID_DATA)?;
    let configuration = buffer.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>();
    if unsafe { QueryServiceConfigW(service, configuration, capacity, &mut required) } == 0 {
        return Err(unsafe { GetLastError() });
    }
    let configuration = unsafe { &*configuration };
    Ok(Configuration {
        service_type: configuration.dwServiceType,
        start_type: configuration.dwStartType,
        error_control: configuration.dwErrorControl,
        binary_path: wide_pointer(&buffer, configuration.lpBinaryPathName)?,
        account: wide_pointer(&buffer, configuration.lpServiceStartName)?,
        display_name: wide_pointer(&buffer, configuration.lpDisplayName)?,
        load_order_group: wide_pointer(&buffer, configuration.lpLoadOrderGroup)?,
        tag_id: configuration.dwTagId,
        dependencies: wide_multi_pointer(&buffer, configuration.lpDependencies)?,
    })
}

fn wide_multi_pointer(buffer: &[usize], value: *const u16) -> Result<Vec<String>, u32> {
    if value.is_null() {
        return Ok(Vec::new());
    }
    wide_multi_units(wide_units_in_buffer(buffer, value)?)
}

fn wide_multi_units(units: &[u16]) -> Result<Vec<String>, u32> {
    let mut entries = Vec::new();
    let mut offset = 0usize;
    loop {
        let remaining = units.get(offset..).ok_or(ERROR_INVALID_DATA)?;
        if remaining.first() == Some(&0) {
            return Ok(entries);
        }
        let length = remaining
            .iter()
            .position(|unit| *unit == 0)
            .ok_or(ERROR_INVALID_DATA)?;
        entries.push(String::from_utf16_lossy(&remaining[..length]));
        offset += length + 1;
    }
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

fn wide_pointer(buffer: &[usize], value: *const u16) -> Result<String, u32> {
    if value.is_null() {
        return Ok(String::new());
    }
    wide_string_units(wide_units_in_buffer(buffer, value)?)
}

fn wide_string_units(units: &[u16]) -> Result<String, u32> {
    let length = units
        .iter()
        .position(|unit| *unit == 0)
        .ok_or(ERROR_INVALID_DATA)?;
    Ok(String::from_utf16_lossy(&units[..length]))
}

fn wide_units_in_buffer(buffer: &[usize], value: *const u16) -> Result<&[u16], u32> {
    let start = buffer.as_ptr() as usize;
    let end = start
        .checked_add(std::mem::size_of_val(buffer))
        .ok_or(ERROR_INVALID_DATA)?;
    let address = value as usize;
    if address < start || address >= end || address % align_of::<u16>() != 0 {
        return Err(ERROR_INVALID_DATA);
    }
    let units = (end - address) / size_of::<u16>();
    Ok(unsafe { std::slice::from_raw_parts(value, units) })
}

pub(in crate::machine_integration::open_service) fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

#[cfg(test)]
mod tests {
    use super::{wide_multi_units, wide_string_units, ERROR_INVALID_DATA};

    #[test]
    fn bounded_scm_strings_require_in_buffer_terminators() {
        assert_eq!(wide_string_units(&[b'a' as u16, 0]).unwrap(), "a");
        assert_eq!(wide_string_units(&[b'a' as u16]), Err(ERROR_INVALID_DATA));
        assert_eq!(
            wide_multi_units(&[b'a' as u16, 0, b'b' as u16, 0, 0]).unwrap(),
            vec!["a".to_owned(), "b".to_owned()]
        );
        assert_eq!(
            wide_multi_units(&[b'a' as u16, 0, b'b' as u16]),
            Err(ERROR_INVALID_DATA)
        );
    }
}
