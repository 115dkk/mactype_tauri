use mactype_service_contract::appinit_mactype_conflict;
use windows_sys::Win32::{
    Foundation::{ERROR_FILE_NOT_FOUND, ERROR_SUCCESS},
    System::Registry::{
        RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_DWORD, RRF_RT_REG_SZ, RRF_SUBKEY_WOW6432KEY,
        RRF_SUBKEY_WOW6464KEY,
    },
};

use crate::ConflictObservation;

pub fn observe_conflict() -> ConflictObservation {
    match [RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY]
        .into_iter()
        .try_fold(false, |conflict, view| {
            observe_view_with(read_enabled(view), || read_dlls(view))
                .map(|current| conflict || current)
        }) {
        Ok(true) => ConflictObservation::Detected,
        Ok(false) => ConflictObservation::Clear,
        Err(()) => ConflictObservation::Unknown,
    }
}

fn observe_view_with<F>(enabled: Result<bool, ()>, read_dlls: F) -> Result<bool, ()>
where
    F: FnOnce() -> Result<Option<Vec<u16>>, ()>,
{
    if !enabled? {
        return Ok(false);
    }

    let value = read_dlls()?;
    appinit_mactype_conflict(true, value.as_deref()).map_err(|_| ())
}

fn read_enabled(view: u32) -> Result<bool, ()> {
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
            (&raw mut enabled).cast(),
            &raw mut bytes,
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

fn read_dlls(view: u32) -> Result<Option<Vec<u16>>, ()> {
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
            &raw mut bytes,
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
            &raw mut bytes,
        )
    };
    if second != ERROR_SUCCESS || !(2..=MAX_APPINIT_BYTES).contains(&bytes) || bytes % 2 != 0 {
        return Err(());
    }
    buffer.truncate(bytes as usize / 2);
    Ok(Some(buffer))
}

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;

    use super::observe_view_with;

    #[test]
    fn disabled_appinit_does_not_read_or_validate_the_dll_list() {
        let read = Cell::new(false);

        let observation = observe_view_with(Ok(false), || {
            read.set(true);
            Err(())
        });

        assert_eq!(observation, Ok(false));
        assert!(!read.get());
    }

    #[test]
    fn enabled_appinit_preserves_dll_read_failures_as_unknown() {
        assert_eq!(observe_view_with(Ok(true), || Err(())), Err(()));
    }
}
