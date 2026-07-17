pub(super) fn appinit_view_conflict(
    enabled: Result<bool, ()>,
    value: Result<Option<Vec<u16>>, ()>,
) -> Result<bool, ()> {
    let enabled = enabled?;
    if !enabled {
        return Ok(false);
    }
    let value = value?;
    mactype_service_contract::appinit_mactype_conflict(true, value.as_deref()).map_err(|_| ())
}

#[cfg(windows)]
fn read_appinit_enabled(view: u32) -> Result<bool, ()> {
    use windows_sys::Win32::{
        Foundation::{ERROR_FILE_NOT_FOUND, ERROR_SUCCESS},
        System::Registry::{RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_DWORD},
    };
    let subkey = wide("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows");
    let value = wide("LoadAppInit_DLLs");
    let mut enabled = 0u32;
    let mut bytes = std::mem::size_of::<u32>() as u32;
    let result = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            subkey.as_ptr(),
            value.as_ptr(),
            RRF_RT_REG_DWORD | view,
            std::ptr::null_mut(),
            (&mut enabled as *mut u32).cast(),
            &mut bytes,
        )
    };
    if result == ERROR_FILE_NOT_FOUND {
        Ok(false)
    } else if result == ERROR_SUCCESS && bytes == std::mem::size_of::<u32>() as u32 {
        Ok(enabled != 0)
    } else {
        Err(())
    }
}

#[cfg(windows)]
fn read_appinit_dlls(view: u32) -> Result<Option<Vec<u16>>, ()> {
    use windows_sys::Win32::{
        Foundation::{ERROR_FILE_NOT_FOUND, ERROR_SUCCESS},
        System::Registry::{RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ},
    };
    const MAX_APPINIT_BYTES: u32 = 64 * 1024;
    let subkey = wide("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows");
    let value = wide("AppInit_DLLs");
    let mut bytes = 0u32;
    let first = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            subkey.as_ptr(),
            value.as_ptr(),
            RRF_RT_REG_SZ | view,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut bytes,
        )
    };
    if first == ERROR_FILE_NOT_FOUND {
        return Ok(None);
    }
    if first != ERROR_SUCCESS || !(2..=MAX_APPINIT_BYTES).contains(&bytes) || bytes % 2 != 0 {
        return Err(());
    }
    let mut buffer = vec![0u16; bytes as usize / 2];
    let second = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            subkey.as_ptr(),
            value.as_ptr(),
            RRF_RT_REG_SZ | view,
            std::ptr::null_mut(),
            buffer.as_mut_ptr().cast(),
            &mut bytes,
        )
    };
    if second != ERROR_SUCCESS || !(2..=MAX_APPINIT_BYTES).contains(&bytes) || bytes % 2 != 0 {
        return Err(());
    }
    buffer.truncate(bytes as usize / 2);
    Ok(Some(buffer))
}

#[cfg(windows)]
pub(super) fn appinit_conflict() -> Result<bool, String> {
    use windows_sys::Win32::System::Registry::{RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY};
    [RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY]
        .into_iter()
        .try_fold(false, |conflict, view| {
            appinit_view_conflict(read_appinit_enabled(view), read_appinit_dlls(view))
                .map(|current| conflict || current)
                .map_err(|_| "AppInit registry state could not be verified".to_owned())
        })
}

pub(crate) fn registry_conflict_detected() -> bool {
    appinit_conflict().unwrap_or(true)
}

#[cfg(not(windows))]
pub(super) fn appinit_conflict() -> Result<bool, String> {
    Ok(false)
}

#[cfg(windows)]
fn wide(value: &str) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    std::ffi::OsStr::new(value)
        .encode_wide()
        .chain(Some(0))
        .collect()
}
