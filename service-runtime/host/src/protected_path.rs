use std::fs::OpenOptions;
use std::io;
use std::io::Read;
use std::path::Path;

pub(crate) const MAX_POINTER_BYTES: u64 = 64 * 1024;

pub(crate) fn read_bounded_regular_file(path: &Path, maximum_bytes: u64) -> io::Result<Vec<u8>> {
    let file = OpenOptions::new().read(true).open(path)?;
    let metadata = file.metadata()?;
    if !metadata.is_file() || metadata.len() == 0 || metadata.len() > maximum_bytes {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "protected file is not a bounded regular file",
        ));
    }
    read_bounded_contents(file, maximum_bytes)
}

pub(crate) fn runtime_pointer_version(bytes: &[u8]) -> Option<String> {
    let value: serde_json::Value = serde_json::from_slice(bytes).ok()?;
    let object = value.as_object()?;
    if object.len() != 2 || object.get("schema").and_then(serde_json::Value::as_u64) != Some(1) {
        return None;
    }
    object
        .get("version")
        .and_then(serde_json::Value::as_str)
        .filter(|version| safe_version_component(version))
        .map(str::to_owned)
}

fn safe_version_component(version: &str) -> bool {
    !version.is_empty()
        && version.len() <= 64
        && version
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'+'))
        && !matches!(version, "." | "..")
}

pub(crate) fn read_bounded_contents(reader: impl Read, maximum_bytes: u64) -> io::Result<Vec<u8>> {
    let mut bytes = Vec::new();
    // The max+1 read must remain even after metadata validation because files can grow in place.
    reader
        .take(maximum_bytes.saturating_add(1))
        .read_to_end(&mut bytes)?;
    if bytes.is_empty() || bytes.len() as u64 > maximum_bytes {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "protected file is outside the bounded size",
        ));
    }
    Ok(bytes)
}

pub(crate) fn has_reparse_ancestor(path: &Path) -> io::Result<bool> {
    for ancestor in path.ancestors().filter(|candidate| candidate.exists()) {
        if is_reparse_point(ancestor)? {
            return Ok(true);
        }
    }
    Ok(false)
}

#[cfg(windows)]
fn is_reparse_point(path: &Path) -> io::Result<bool> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{
        GetFileAttributesW, FILE_ATTRIBUTE_REPARSE_POINT, INVALID_FILE_ATTRIBUTES,
    };
    let path = path
        .as_os_str()
        .encode_wide()
        .chain(Some(0))
        .collect::<Vec<_>>();
    let attributes = unsafe { GetFileAttributesW(path.as_ptr()) };
    if attributes == INVALID_FILE_ATTRIBUTES {
        return Err(io::Error::last_os_error());
    }
    Ok(attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0)
}

#[cfg(not(windows))]
fn is_reparse_point(path: &Path) -> io::Result<bool> {
    Ok(std::fs::symlink_metadata(path)?.file_type().is_symlink())
}

#[cfg(test)]
mod tests {
    use std::io::Cursor;

    use super::read_bounded_contents;

    #[test]
    fn bounded_reader_rejects_a_stream_that_grows_after_the_metadata_size_check() {
        assert!(read_bounded_contents(Cursor::new(vec![b'x'; 65]), 64).is_err());
    }
}
