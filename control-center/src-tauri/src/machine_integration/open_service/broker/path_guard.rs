use std::{ffi::OsStr, os::windows::ffi::OsStrExt, path::Path};

pub(super) fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

pub(in crate::machine_integration::open_service) fn reject_reparse_ancestors(
    path: &Path,
) -> Result<(), String> {
    use windows_sys::Win32::Storage::FileSystem::{
        GetFileAttributesW, FILE_ATTRIBUTE_REPARSE_POINT, INVALID_FILE_ATTRIBUTES,
    };
    for ancestor in path.ancestors().filter(|candidate| candidate.exists()) {
        let value = wide(ancestor.as_os_str());
        let attributes = unsafe { GetFileAttributesW(value.as_ptr()) };
        if attributes == INVALID_FILE_ATTRIBUTES {
            return Err(std::io::Error::last_os_error().to_string());
        }
        if attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return Err("reparse points are forbidden in the broker staging path".to_owned());
        }
    }
    Ok(())
}
