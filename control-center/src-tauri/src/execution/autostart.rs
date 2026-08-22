use std::env;

#[cfg(windows)]
fn wide(value: &str) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    std::ffi::OsStr::new(value)
        .encode_wide()
        .chain(Some(0))
        .collect()
}

#[cfg(windows)]
fn read_registry_string(
    root: windows_sys::Win32::System::Registry::HKEY,
    subkey: &str,
    value: &str,
    flags: u32,
) -> Option<String> {
    use windows_sys::Win32::{Foundation::ERROR_SUCCESS, System::Registry::RegGetValueW};
    let subkey = wide(subkey);
    let value = wide(value);
    let mut bytes = 0u32;
    let result = unsafe {
        RegGetValueW(
            root,
            subkey.as_ptr(),
            value.as_ptr(),
            flags,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut bytes,
        )
    };
    let units = registry_wide_units(bytes)?;
    if result != ERROR_SUCCESS {
        return None;
    }
    let capacity_bytes = bytes;
    let mut buffer = vec![0u16; units];
    let mut read_bytes = capacity_bytes;
    let result = unsafe {
        RegGetValueW(
            root,
            subkey.as_ptr(),
            value.as_ptr(),
            flags,
            std::ptr::null_mut(),
            buffer.as_mut_ptr().cast(),
            &mut read_bytes,
        )
    };
    if result != ERROR_SUCCESS || read_bytes > capacity_bytes {
        return None;
    }
    buffer.truncate(registry_wide_units(read_bytes)?);
    let length = buffer
        .iter()
        .position(|unit| *unit == 0)
        .unwrap_or(buffer.len());
    Some(String::from_utf16_lossy(&buffer[..length]))
}

#[cfg(windows)]
fn registry_wide_units(bytes: u32) -> Option<usize> {
    const MAX_REGISTRY_STRING_BYTES: u32 = 64 * 1024;
    ((2..=MAX_REGISTRY_STRING_BYTES).contains(&bytes) && bytes % 2 == 0)
        .then_some(bytes as usize / 2)
}

#[cfg(all(test, windows))]
mod tests {
    use super::registry_wide_units;

    #[test]
    fn registry_string_size_must_be_even_and_bounded() {
        assert_eq!(registry_wide_units(2), Some(1));
        assert_eq!(registry_wide_units(64 * 1024), Some(32 * 1024));
        assert_eq!(registry_wide_units(0), None);
        assert_eq!(registry_wide_units(1), None);
        assert_eq!(registry_wide_units(3), None);
        assert_eq!(registry_wide_units(64 * 1024 + 2), None);
    }
}

#[cfg(windows)]
pub(super) fn autostart_value() -> Option<String> {
    use windows_sys::Win32::System::Registry::{HKEY_CURRENT_USER, RRF_RT_REG_SZ};
    read_registry_string(
        HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        "MacTypeControlCenter",
        RRF_RT_REG_SZ,
    )
}

#[cfg(not(windows))]
pub(super) fn autostart_value() -> Option<String> {
    None
}

#[cfg(windows)]
pub(super) fn set_autostart(enabled: bool) -> Result<bool, String> {
    use windows_sys::Win32::{
        Foundation::{ERROR_FILE_NOT_FOUND, ERROR_SUCCESS},
        System::Registry::{RegDeleteKeyValueW, RegSetKeyValueW, HKEY_CURRENT_USER, REG_SZ},
    };
    let subkey = wide("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    let name = wide("MacTypeControlCenter");
    let result = if enabled {
        let executable = env::current_exe().map_err(|error| error.to_string())?;
        let command = wide(&format!("\"{}\" --tray", executable.display()));
        unsafe {
            RegSetKeyValueW(
                HKEY_CURRENT_USER,
                subkey.as_ptr(),
                name.as_ptr(),
                REG_SZ,
                command.as_ptr().cast(),
                (command.len() * 2) as u32,
            )
        }
    } else {
        unsafe { RegDeleteKeyValueW(HKEY_CURRENT_USER, subkey.as_ptr(), name.as_ptr()) }
    };
    if result == ERROR_SUCCESS || (!enabled && result == ERROR_FILE_NOT_FOUND) {
        Ok(autostart_value().is_some())
    } else {
        Err(format!(
            "autostart registry update failed with Windows error {result}"
        ))
    }
}

#[cfg(not(windows))]
pub(super) fn set_autostart(_enabled: bool) -> Result<bool, String> {
    Err("autostart is supported only on Windows".to_owned())
}
