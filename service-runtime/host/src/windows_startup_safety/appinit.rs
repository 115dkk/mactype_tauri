use std::mem::size_of;
use std::ptr;

use mactype_service_contract::{appinit_mactype_conflict, StructuredServiceError};
use windows_sys::Win32::Foundation::{ERROR_FILE_NOT_FOUND, ERROR_SUCCESS};
use windows_sys::Win32::System::Registry::{
    RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_LOCAL_MACHINE, KEY_READ, REG_DWORD,
    REG_EXPAND_SZ, REG_SZ,
};

const APPINIT_KEY: &str = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows";
const APPINIT_ENABLED_VALUE: &str = "LoadAppInit_DLLs";
const APPINIT_DLLS_VALUE: &str = "AppInit_DLLs";

struct RegistryKey(HKEY);

impl Drop for RegistryKey {
    fn drop(&mut self) {
        unsafe { RegCloseKey(self.0) };
    }
}

pub(super) fn mactype_enabled(view: u32) -> Result<bool, StructuredServiceError> {
    let path = wide_null(APPINIT_KEY);
    let mut key = ptr::null_mut();
    let result = unsafe {
        RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            path.as_ptr(),
            0,
            KEY_READ | view,
            &mut key,
        )
    };
    if result != ERROR_SUCCESS {
        return Err(service_error(
            "appinit-inspection-failed",
            "an AppInit registry view could not be opened",
            Some(result as i32),
        ));
    }
    let key = RegistryKey(key);
    if !read_enabled(&key)? {
        return Ok(false);
    }
    let value = read_dlls(&key)?;
    appinit_mactype_conflict(true, value.as_deref()).map_err(|_| {
        service_error(
            "appinit-inspection-failed",
            "the enabled AppInit_DLLs value is malformed",
            None,
        )
    })
}

fn read_enabled(key: &RegistryKey) -> Result<bool, StructuredServiceError> {
    let name = wide_null(APPINIT_ENABLED_VALUE);
    let mut value_type = 0;
    let mut value = 0u32;
    let mut size = size_of::<u32>() as u32;
    let result = unsafe {
        RegQueryValueExW(
            key.0,
            name.as_ptr(),
            ptr::null(),
            &mut value_type,
            (&mut value as *mut u32).cast::<u8>(),
            &mut size,
        )
    };
    if result == ERROR_FILE_NOT_FOUND {
        return Ok(false);
    }
    if result != ERROR_SUCCESS || value_type != REG_DWORD || size != size_of::<u32>() as u32 {
        return Err(service_error(
            "appinit-inspection-failed",
            "the AppInit enable flag could not be read as a DWORD",
            Some(result as i32),
        ));
    }
    Ok(value != 0)
}

fn read_dlls(key: &RegistryKey) -> Result<Option<Vec<u16>>, StructuredServiceError> {
    let name = wide_null(APPINIT_DLLS_VALUE);
    let mut value_type = 0;
    let mut size = 0;
    let result = unsafe {
        RegQueryValueExW(
            key.0,
            name.as_ptr(),
            ptr::null(),
            &mut value_type,
            ptr::null_mut(),
            &mut size,
        )
    };
    if result == ERROR_FILE_NOT_FOUND {
        return Ok(None);
    }
    if result != ERROR_SUCCESS
        || !matches!(value_type, REG_SZ | REG_EXPAND_SZ)
        || size == 0
        || size % 2 != 0
    {
        return Err(service_error(
            "appinit-inspection-failed",
            "the enabled AppInit_DLLs value type or size is invalid",
            Some(result as i32),
        ));
    }

    let mut value = vec![0u16; size as usize / 2];
    let mut actual_type = 0;
    let mut actual_size = size;
    let result = unsafe {
        RegQueryValueExW(
            key.0,
            name.as_ptr(),
            ptr::null(),
            &mut actual_type,
            value.as_mut_ptr().cast::<u8>(),
            &mut actual_size,
        )
    };
    if result != ERROR_SUCCESS
        || actual_type != value_type
        || actual_size == 0
        || actual_size % 2 != 0
        || actual_size > size
    {
        return Err(service_error(
            "appinit-inspection-failed",
            "the enabled AppInit_DLLs value could not be read safely",
            Some(result as i32),
        ));
    }
    value.truncate(actual_size as usize / 2);
    Ok(Some(value))
}

fn wide_null(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}

fn service_error(code: &str, message: &str, win32_error: Option<i32>) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error: win32_error.map(|code| code as u32),
    }
}
