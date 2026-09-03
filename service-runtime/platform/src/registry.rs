//! Read-only registry access with bounded buffers.

use std::io;
use std::ptr::{null, null_mut};

use windows_sys::Win32::Foundation::{
    ERROR_FILE_NOT_FOUND, ERROR_MORE_DATA, ERROR_NO_MORE_ITEMS, ERROR_PATH_NOT_FOUND, ERROR_SUCCESS,
};
use windows_sys::Win32::System::Registry::{
    RegCloseKey, RegEnumValueW, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_CURRENT_USER,
    HKEY_LOCAL_MACHINE, KEY_READ, KEY_WOW64_32KEY, KEY_WOW64_64KEY, REG_DWORD, REG_EXPAND_SZ,
    REG_SZ,
};

use crate::wide::wide_null;

const MAX_VALUE_NAME_UNITS: usize = 16_384;
const MAX_VALUE_BYTES: usize = 1_048_576;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RegistryRoot {
    LocalMachine,
    CurrentUser,
}

impl RegistryRoot {
    fn raw(self) -> HKEY {
        match self {
            Self::LocalMachine => HKEY_LOCAL_MACHINE,
            Self::CurrentUser => HKEY_CURRENT_USER,
        }
    }
}

/// The registry view (WOW64 redirection) to open.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RegistryView {
    Native32,
    Native64,
}

impl RegistryView {
    fn flag(self) -> u32 {
        match self {
            Self::Native32 => KEY_WOW64_32KEY,
            Self::Native64 => KEY_WOW64_64KEY,
        }
    }
}

/// The data of an enumerated value, typed by the registry's own type tag.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum RegistryValueData {
    /// `REG_SZ` or `REG_EXPAND_SZ`, unexpanded, with trailing NULs removed.
    /// `None` when the bytes were not valid UTF-16.
    String(Option<String>),
    Dword(u32),
    Other {
        kind: u32,
    },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RegistryValue {
    pub name: String,
    pub data: RegistryValueData,
}

/// An open registry key with read access only.
#[derive(Debug)]
pub struct RegistryKey(HKEY);

impl RegistryKey {
    /// Opens `path` under `root` in `view`. `Ok(None)` when the key does not
    /// exist; any other failure carries the Win32 status.
    pub fn open(root: RegistryRoot, path: &str, view: RegistryView) -> io::Result<Option<Self>> {
        let path = wide_null(path);
        let mut key = null_mut();
        // SAFETY: `path` is NUL-terminated and `key` is a local out pointer.
        let status = unsafe {
            RegOpenKeyExW(
                root.raw(),
                path.as_ptr(),
                0,
                KEY_READ | view.flag(),
                &mut key,
            )
        };
        if matches!(status, ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND) {
            return Ok(None);
        }
        if status != ERROR_SUCCESS || key.is_null() {
            return Err(io::Error::from_raw_os_error(status as i32));
        }
        Ok(Some(Self(key)))
    }

    /// Reads a `REG_DWORD` value. `Ok(None)` when the value does not exist; a
    /// value of another type or size is an `InvalidData` error.
    pub fn read_dword(&self, name: &str) -> io::Result<Option<u32>> {
        let name = wide_null(name);
        let mut kind = 0;
        let mut value = 0_u32;
        let mut size = std::mem::size_of::<u32>() as u32;
        // SAFETY: the key is open; `name` is NUL-terminated; the data pointer
        // and `size` describe exactly the local `u32`.
        let status = unsafe {
            RegQueryValueExW(
                self.0,
                name.as_ptr(),
                null(),
                &mut kind,
                (&mut value as *mut u32).cast::<u8>(),
                &mut size,
            )
        };
        if status == ERROR_FILE_NOT_FOUND {
            return Ok(None);
        }
        if status != ERROR_SUCCESS {
            return Err(io::Error::from_raw_os_error(status as i32));
        }
        if kind != REG_DWORD || size != std::mem::size_of::<u32>() as u32 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "registry value is not a DWORD",
            ));
        }
        Ok(Some(value))
    }

    /// Reads a `REG_SZ` or `REG_EXPAND_SZ` value as raw UTF-16 units without
    /// expansion. `Ok(None)` when the value does not exist. The result is
    /// bounded to one megabyte and keeps any embedded or trailing NULs so the
    /// caller decides how strictly to parse it.
    pub fn read_string_units(&self, name: &str) -> io::Result<Option<Vec<u16>>> {
        let name = wide_null(name);
        let mut kind = 0;
        let mut size = 0;
        // SAFETY: the key is open; `name` is NUL-terminated; a null data
        // pointer asks only for the size, which lands in the local `size`.
        let status = unsafe {
            RegQueryValueExW(
                self.0,
                name.as_ptr(),
                null(),
                &mut kind,
                null_mut(),
                &mut size,
            )
        };
        if status == ERROR_FILE_NOT_FOUND {
            return Ok(None);
        }
        if status != ERROR_SUCCESS {
            return Err(io::Error::from_raw_os_error(status as i32));
        }
        if !matches!(kind, REG_SZ | REG_EXPAND_SZ)
            || size == 0
            || size % 2 != 0
            || size as usize > MAX_VALUE_BYTES
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "registry value is not a bounded string",
            ));
        }
        let mut value = vec![0_u16; size as usize / 2];
        let mut actual_kind = 0;
        let mut actual_size = size;
        // SAFETY: the key is open; `value` is writable for `actual_size` bytes,
        // which is the capacity passed alongside it.
        let status = unsafe {
            RegQueryValueExW(
                self.0,
                name.as_ptr(),
                null(),
                &mut actual_kind,
                value.as_mut_ptr().cast::<u8>(),
                &mut actual_size,
            )
        };
        if status != ERROR_SUCCESS
            || actual_kind != kind
            || actual_size == 0
            || actual_size % 2 != 0
            || actual_size > size
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "registry value changed while it was being read",
            ));
        }
        value.truncate(actual_size as usize / 2);
        Ok(Some(value))
    }

    /// Enumerates every value of the key. A name or datum outside the fixed
    /// bounds ends the enumeration with an error rather than a partial list.
    pub fn values(&self) -> io::Result<Vec<RegistryValue>> {
        let mut values = Vec::new();
        let mut name = vec![0_u16; MAX_VALUE_NAME_UNITS + 1];
        let mut data = vec![0_u8; MAX_VALUE_BYTES];
        for index in 0.. {
            let mut name_length = MAX_VALUE_NAME_UNITS as u32;
            let mut kind = 0_u32;
            let mut data_length = data.len() as u32;
            // SAFETY: the key is open; `name` and `data` are writable for the
            // lengths passed alongside them, and both lengths are updated to
            // what was written.
            let status = unsafe {
                RegEnumValueW(
                    self.0,
                    index,
                    name.as_mut_ptr(),
                    &mut name_length,
                    null(),
                    &mut kind,
                    data.as_mut_ptr(),
                    &mut data_length,
                )
            };
            if status == ERROR_NO_MORE_ITEMS {
                break;
            }
            if status == ERROR_MORE_DATA {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "registry value exceeds the fixed bound",
                ));
            }
            if status != ERROR_SUCCESS
                || name_length as usize > MAX_VALUE_NAME_UNITS
                || data_length as usize > data.len()
            {
                return Err(io::Error::from_raw_os_error(status as i32));
            }
            let name = String::from_utf16(&name[..name_length as usize]).map_err(|_| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "registry value name is not UTF-16",
                )
            })?;
            let bytes = &data[..data_length as usize];
            let data = match kind {
                REG_SZ | REG_EXPAND_SZ => RegistryValueData::String(decode_string(bytes)),
                REG_DWORD if bytes.len() == 4 => RegistryValueData::Dword(u32::from_le_bytes([
                    bytes[0], bytes[1], bytes[2], bytes[3],
                ])),
                other => RegistryValueData::Other { kind: other },
            };
            values.push(RegistryValue { name, data });
        }
        Ok(values)
    }
}

impl Drop for RegistryKey {
    fn drop(&mut self) {
        // SAFETY: the key was opened by `RegOpenKeyExW` and is closed once.
        unsafe { RegCloseKey(self.0) };
    }
}

fn decode_string(bytes: &[u8]) -> Option<String> {
    if bytes.len() % 2 != 0 {
        return None;
    }
    let mut units: Vec<u16> = bytes
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect();
    while units.last() == Some(&0) {
        units.pop();
    }
    String::from_utf16(&units).ok()
}

#[cfg(test)]
mod tests {
    use super::{decode_string, RegistryKey, RegistryRoot, RegistryValueData, RegistryView};

    #[test]
    fn the_windows_version_key_reads_through_every_accessor() {
        let key = RegistryKey::open(
            RegistryRoot::LocalMachine,
            r"SOFTWARE\Microsoft\Windows NT\CurrentVersion",
            RegistryView::Native64,
        )
        .unwrap()
        .expect("the CurrentVersion key always exists");
        let product = key.read_string_units("ProductName").unwrap().unwrap();
        assert!(!product.is_empty());
        assert!(key
            .read_dword("CurrentMajorVersionNumber")
            .unwrap()
            .is_some());
        assert!(key.read_dword("ProductName").is_err());
        assert_eq!(key.read_dword("mactype-platform-missing").unwrap(), None);
        let values = key.values().unwrap();
        assert!(values.iter().any(|value| {
            value.name == "ProductName" && matches!(value.data, RegistryValueData::String(Some(_)))
        }));
    }

    #[test]
    fn a_missing_key_is_none_not_an_error() {
        assert!(RegistryKey::open(
            RegistryRoot::CurrentUser,
            r"SOFTWARE\mactype-platform-missing-key",
            RegistryView::Native32,
        )
        .unwrap()
        .is_none());
    }

    #[test]
    fn registry_strings_drop_trailing_nuls_and_reject_odd_lengths() {
        let mut bytes = Vec::new();
        for unit in "run".encode_utf16().chain([0, 0]) {
            bytes.extend_from_slice(&unit.to_le_bytes());
        }
        assert_eq!(decode_string(&bytes).as_deref(), Some("run"));
        assert_eq!(decode_string(&bytes[..5]), None);
    }
}
