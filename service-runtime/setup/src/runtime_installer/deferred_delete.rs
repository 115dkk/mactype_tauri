use std::fs;
use std::path::Path;

use crate::SetupError;

pub(super) fn remove_file_or_defer(path: &Path, operation: &'static str) -> Result<(), SetupError> {
    remove_or_defer(path, false).map_err(|error| error.at_machine_path(operation, path))
}

pub(super) fn remove_directory_or_defer(
    path: &Path,
    operation: &'static str,
) -> Result<(), SetupError> {
    remove_or_defer(path, true).map_err(|error| error.at_machine_path(operation, path))
}

pub(super) fn defer_directory(path: &Path, operation: &'static str) -> Result<(), SetupError> {
    defer_delete_after_reboot(path)
        .map_err(SetupError::Io)
        .map_err(|error| error.at_machine_path(operation, path))
}

fn remove_or_defer(path: &Path, directory: bool) -> Result<(), SetupError> {
    let removal = if directory {
        fs::remove_dir(path)
    } else {
        fs::remove_file(path)
    };
    match removal {
        Ok(()) => Ok(()),
        Err(removal_error) => defer_delete_after_reboot(path).map_err(|defer_error| {
            SetupError::CleanupUnknown(format!(
                "verified runtime removal failed ({removal_error}) and could not be deferred until reboot ({defer_error})"
            ))
        }),
    }
}

#[cfg(windows)]
fn defer_delete_after_reboot(path: &Path) -> Result<(), std::io::Error> {
    use std::os::windows::ffi::OsStrExt;
    use std::ptr;

    use windows_sys::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_DELAY_UNTIL_REBOOT};

    let mut wide = path.as_os_str().encode_wide().collect::<Vec<_>>();
    wide.push(0);
    if unsafe { MoveFileExW(wide.as_ptr(), ptr::null(), MOVEFILE_DELAY_UNTIL_REBOOT) } == 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(not(windows))]
fn defer_delete_after_reboot(_path: &Path) -> Result<(), std::io::Error> {
    Err(std::io::Error::new(
        std::io::ErrorKind::Unsupported,
        "deferred deletion is available only on Windows",
    ))
}
