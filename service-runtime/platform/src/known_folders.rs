//! Known-folder lookups.

use std::ffi::c_void;
use std::io;
use std::os::windows::ffi::OsStringExt;
use std::path::PathBuf;
use std::ptr::null_mut;

use windows_sys::core::GUID;
use windows_sys::Win32::System::Com::CoTaskMemFree;
use windows_sys::Win32::UI::Shell::{
    FOLDERID_ProgramData, FOLDERID_ProgramFiles, FOLDERID_Startup, SHGetKnownFolderPath,
    KF_FLAG_DEFAULT,
};

use crate::wide::bounded_units;

const MAX_FOLDER_PATH_UNITS: usize = 32_768;

/// The folders the service resolves. Only these three exist as values, so a
/// caller cannot ask the shell for an arbitrary folder identifier.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum KnownFolder {
    ProgramFiles,
    ProgramData,
    /// The current user's Startup folder; needs a COM apartment on the thread.
    Startup,
}

impl KnownFolder {
    fn identifier(self) -> &'static GUID {
        match self {
            Self::ProgramFiles => &FOLDERID_ProgramFiles,
            Self::ProgramData => &FOLDERID_ProgramData,
            Self::Startup => &FOLDERID_Startup,
        }
    }
}

/// Resolves `folder` to its current path.
pub fn known_folder_path(folder: KnownFolder) -> io::Result<PathBuf> {
    let mut raw = null_mut();
    // SAFETY: the identifier is a static GUID, the token is null, and `raw`
    // is a live out pointer that receives a CoTaskMemAlloc string on success.
    let result = unsafe {
        SHGetKnownFolderPath(
            folder.identifier(),
            KF_FLAG_DEFAULT as u32,
            null_mut(),
            &mut raw,
        )
    };
    if result < 0 || raw.is_null() {
        return Err(io::Error::from_raw_os_error(result));
    }
    // SAFETY: the shell returned a NUL-terminated string in `raw`; it stays
    // valid until the CoTaskMemFree below.
    let path = unsafe { bounded_units(raw, MAX_FOLDER_PATH_UNITS) }
        .map(|units| PathBuf::from(std::ffi::OsString::from_wide(units)));
    // SAFETY: `raw` was allocated by the shell for this call and is freed once.
    unsafe { CoTaskMemFree(raw as *const c_void) };
    path.ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "the shell returned an unterminated known folder path",
        )
    })
}

#[cfg(test)]
mod tests {
    use super::{known_folder_path, KnownFolder};

    #[test]
    fn machine_folders_resolve_to_absolute_directories() {
        for folder in [KnownFolder::ProgramFiles, KnownFolder::ProgramData] {
            let path = known_folder_path(folder).unwrap();
            assert!(path.is_absolute(), "{path:?}");
            assert!(path.is_dir(), "{path:?}");
        }
    }
}
