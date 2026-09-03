//! File-system calls the standard library does not expose in the form the
//! service needs: attribute reads without following reparse points,
//! write-through replacement, reboot-deferred deletion, and deleting a file
//! through the handle that already verified its contents.

use std::fs::File;
use std::io;
use std::os::windows::io::AsRawHandle;
use std::path::Path;
use std::ptr::null;

use windows_sys::Win32::Storage::FileSystem::{
    FileDispositionInfo, GetFileAttributesW, MoveFileExW, SetFileInformationByHandle,
    FILE_ATTRIBUTE_REPARSE_POINT, FILE_DISPOSITION_INFO, INVALID_FILE_ATTRIBUTES,
    MOVEFILE_DELAY_UNTIL_REBOOT, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
};

use crate::wide::wide_path;

/// The raw attribute bits of `path`, read without following reparse points.
pub fn file_attributes(path: &Path) -> io::Result<u32> {
    let wide = wide_path(path);
    // SAFETY: `wide` is NUL-terminated and outlives the call.
    let attributes = unsafe { GetFileAttributesW(wide.as_ptr()) };
    if attributes == INVALID_FILE_ATTRIBUTES {
        return Err(io::Error::last_os_error());
    }
    Ok(attributes)
}

/// Whether `path` itself is a reparse point (symlink, junction, mount point).
pub fn is_reparse_point(path: &Path) -> io::Result<bool> {
    Ok(file_attributes(path)? & FILE_ATTRIBUTE_REPARSE_POINT != 0)
}

/// Moves `source` over `destination`, replacing it, with the move committed to
/// disk before the call returns.
pub fn replace_file(source: &Path, destination: &Path) -> io::Result<()> {
    let source = wide_path(source);
    let destination = wide_path(destination);
    // SAFETY: both strings are NUL-terminated and outlive the call.
    if unsafe {
        MoveFileExW(
            source.as_ptr(),
            destination.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

/// Schedules `path` for deletion at the next reboot.
pub fn delay_delete_until_reboot(path: &Path) -> io::Result<()> {
    let wide = wide_path(path);
    // SAFETY: the source is NUL-terminated and outlives the call; a null
    // destination with this flag means "delete".
    if unsafe { MoveFileExW(wide.as_ptr(), null(), MOVEFILE_DELAY_UNTIL_REBOOT) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

/// Marks the file behind `file` for deletion when its last handle closes. The
/// handle that read and verified the contents is the one that deletes, so no
/// other file can be substituted between the check and the removal. The
/// handle must have been opened with `DELETE` access.
pub fn mark_open_file_for_deletion(file: &File) -> io::Result<()> {
    let disposition = FILE_DISPOSITION_INFO { DeleteFile: true };
    // SAFETY: the handle comes from a live `File`; the buffer pointer and
    // length describe exactly the local disposition structure.
    if unsafe {
        SetFileInformationByHandle(
            file.as_raw_handle(),
            FileDispositionInfo,
            (&disposition as *const FILE_DISPOSITION_INFO).cast(),
            std::mem::size_of::<FILE_DISPOSITION_INFO>() as u32,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{file_attributes, is_reparse_point, mark_open_file_for_deletion, replace_file};

    #[test]
    fn attributes_replacement_and_handle_deletion_act_on_real_files() {
        let directory = tempfile::tempdir().unwrap();
        let first = directory.path().join("first.txt");
        let second = directory.path().join("second.txt");
        std::fs::write(&first, b"first").unwrap();
        std::fs::write(&second, b"second").unwrap();

        assert!(file_attributes(&first).unwrap() != 0);
        assert!(!is_reparse_point(&first).unwrap());
        assert!(file_attributes(&directory.path().join("missing")).is_err());

        replace_file(&first, &second).unwrap();
        assert!(!first.exists());
        assert_eq!(std::fs::read(&second).unwrap(), b"first");

        use std::os::windows::fs::OpenOptionsExt;
        const GENERIC_READ: u32 = 0x8000_0000;
        const DELETE: u32 = 0x0001_0000;
        let file = std::fs::OpenOptions::new()
            .access_mode(GENERIC_READ | DELETE)
            .open(&second)
            .unwrap();
        mark_open_file_for_deletion(&file).unwrap();
        drop(file);
        assert!(!second.exists());
    }
}
