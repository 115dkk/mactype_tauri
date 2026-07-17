use std::fs;
use std::io::{self, Read};
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::thread;
use std::time::Duration;

use mactype_service_contract::MachinePaths;

use super::stop_requested;

pub(super) fn spawn_crash_once_adapter(paths: MachinePaths) {
    let Some(data_root) = paths.active_profile().parent().map(Path::to_owned) else {
        return;
    };
    thread::spawn(move || {
        let request = data_root.join("ci-test-adapter").join("crash-once.request");
        while !stop_requested() {
            if consume_crash_once_request(&request).unwrap_or(false) {
                unsafe {
                    windows_sys::Win32::System::Threading::TerminateProcess(
                        windows_sys::Win32::System::Threading::GetCurrentProcess(),
                        0x4d54_0001,
                    );
                }
                return;
            }
            thread::sleep(Duration::from_millis(100));
        }
    });
}

fn consume_crash_once_request(request: &Path) -> io::Result<bool> {
    if !request.exists() {
        return Ok(false);
    }
    reject_reparse_ancestors(request)?;
    let mut file = fs::OpenOptions::new().read(true).open(request)?;
    let metadata = file.metadata()?;
    if !metadata.is_file() || metadata.len() == 0 || metadata.len() > 64 {
        return Ok(false);
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.by_ref().take(65).read_to_end(&mut bytes)?;
    if !valid_crash_once_marker(&bytes) {
        return Ok(false);
    }
    drop(file);
    let consumed = request.with_file_name("crash-once.consumed");
    if consumed.exists() {
        reject_reparse_ancestors(&consumed)?;
        let metadata = fs::metadata(&consumed)?;
        if !metadata.is_file() || metadata.len() > 64 {
            return Ok(false);
        }
        fs::remove_file(&consumed)?;
    }
    fs::rename(request, consumed)?;
    Ok(true)
}

pub(super) fn valid_crash_once_marker(bytes: &[u8]) -> bool {
    bytes == b"mactype-ci-crash-once\n"
}

fn reject_reparse_ancestors(path: &Path) -> io::Result<()> {
    use windows_sys::Win32::Storage::FileSystem::{
        GetFileAttributesW, FILE_ATTRIBUTE_REPARSE_POINT, INVALID_FILE_ATTRIBUTES,
    };
    for ancestor in path.ancestors().filter(|candidate| candidate.exists()) {
        let wide = wide_path(ancestor);
        let attributes = unsafe { GetFileAttributesW(wide.as_ptr()) };
        if attributes == INVALID_FILE_ATTRIBUTES {
            return Err(io::Error::last_os_error());
        }
        if attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "ci-test-adapter crash marker path contains a reparse point",
            ));
        }
    }
    Ok(())
}

fn wide_path(path: &Path) -> Vec<u16> {
    path.as_os_str().encode_wide().chain(Some(0)).collect()
}
