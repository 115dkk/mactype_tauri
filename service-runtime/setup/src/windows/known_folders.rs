use std::ffi::c_void;
use std::io;
use std::os::windows::ffi::OsStringExt;
use std::path::PathBuf;
use std::ptr;

use mactype_service_contract::MachinePaths;
use windows_sys::core::GUID;
use windows_sys::Win32::System::Com::CoTaskMemFree;
use windows_sys::Win32::UI::Shell::{
    FOLDERID_ProgramData, FOLDERID_ProgramFiles, SHGetKnownFolderPath, KF_FLAG_DEFAULT,
};

use crate::SetupError;

pub fn machine_paths() -> Result<MachinePaths, SetupError> {
    let program_files = known_folder(&FOLDERID_ProgramFiles)?;
    let program_data = known_folder(&FOLDERID_ProgramData)?;
    MachinePaths::from_trusted_os_roots(&program_files, &program_data)
        .map_err(|error| SetupError::Runtime(error.to_string()))
}

fn known_folder(identifier: &GUID) -> Result<PathBuf, SetupError> {
    let mut raw = ptr::null_mut();
    let result = unsafe {
        SHGetKnownFolderPath(
            identifier,
            KF_FLAG_DEFAULT as u32,
            ptr::null_mut(),
            &mut raw,
        )
    };
    if result < 0 || raw.is_null() {
        return Err(SetupError::Io(io::Error::from_raw_os_error(result)));
    }
    let length = unsafe {
        let mut length = 0usize;
        while *raw.add(length) != 0 {
            length += 1;
        }
        length
    };
    let path = PathBuf::from(std::ffi::OsString::from_wide(unsafe {
        std::slice::from_raw_parts(raw, length)
    }));
    unsafe {
        CoTaskMemFree(raw as *const c_void);
    }
    Ok(path)
}
