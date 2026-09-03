//! UTF-16 conversions shared by the platform adapters. Everything here is
//! bounds-driven: a NUL-terminated buffer is only ever read up to a caller
//! supplied maximum, so a missing terminator cannot turn into a read past the
//! allocation.

use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;

/// Encodes `value` as NUL-terminated UTF-16 for a Win32 `LPCWSTR` argument.
pub(crate) fn wide_null(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

/// Encodes a path as NUL-terminated UTF-16.
pub(crate) fn wide_path(path: &Path) -> Vec<u16> {
    wide_null(path.as_os_str())
}

/// Reads a NUL-terminated UTF-16 string of at most `maximum` units from
/// `pointer`. Returns `None` for a null pointer or when no terminator appears
/// inside the bound, so callers see "unreadable" rather than a truncated value.
///
/// # Safety
///
/// `pointer` must either be null or point at readable memory that stays valid
/// for `maximum` UTF-16 units or up to and including its NUL terminator,
/// whichever comes first.
pub(crate) unsafe fn bounded_units<'a>(pointer: *const u16, maximum: usize) -> Option<&'a [u16]> {
    if pointer.is_null() {
        return None;
    }
    let mut length = 0_usize;
    // SAFETY: the caller guarantees the buffer is readable up to `maximum`
    // units or its terminator; the loop stops at whichever comes first.
    while length < maximum && unsafe { *pointer.add(length) } != 0 {
        length += 1;
    }
    if length == maximum {
        return None;
    }
    // SAFETY: every unit in `..length` was just read through the same pointer.
    Some(unsafe { std::slice::from_raw_parts(pointer, length) })
}

/// Converts a bounded UTF-16 read into a lossy `String`.
///
/// # Safety
///
/// Same contract as [`bounded_units`].
pub(crate) unsafe fn bounded_string_lossy(pointer: *const u16, maximum: usize) -> Option<String> {
    // SAFETY: forwarded verbatim to the caller's guarantee.
    unsafe { bounded_units(pointer, maximum) }.map(String::from_utf16_lossy)
}

/// Converts a bounded UTF-16 read into a `String`, rejecting invalid UTF-16.
///
/// # Safety
///
/// Same contract as [`bounded_units`].
pub(crate) unsafe fn bounded_string(pointer: *const u16, maximum: usize) -> Option<String> {
    // SAFETY: forwarded verbatim to the caller's guarantee.
    unsafe { bounded_units(pointer, maximum) }.and_then(|units| String::from_utf16(units).ok())
}

/// Splits a `REG_MULTI_SZ`-style double-NUL-terminated block, read from an
/// already bounded slice, into its strings.
pub(crate) fn multi_strings(units: &[u16]) -> Vec<String> {
    units
        .split(|unit| *unit == 0)
        .take_while(|entry| !entry.is_empty())
        .map(String::from_utf16_lossy)
        .collect()
}

/// Truncates an owned buffer at its first NUL, for APIs that fill a caller
/// buffer without returning the length.
pub(crate) fn nul_terminated(units: &[u16]) -> Option<String> {
    let length = units.iter().position(|unit| *unit == 0)?;
    String::from_utf16(&units[..length]).ok()
}

#[cfg(test)]
mod tests {
    use super::{bounded_units, multi_strings, nul_terminated, wide_null};

    #[test]
    fn bounded_reads_stop_at_the_terminator_and_reject_missing_ones() {
        let terminated: Vec<u16> = "abc".encode_utf16().chain(Some(0)).collect();
        // SAFETY: the buffer is a live Vec with a terminator inside the bound.
        let read = unsafe { bounded_units(terminated.as_ptr(), 8) }.unwrap();
        assert_eq!(String::from_utf16_lossy(read), "abc");

        let unterminated: Vec<u16> = "abcd".encode_utf16().collect();
        // SAFETY: the bound equals the buffer length, so no unit past it is read.
        assert!(unsafe { bounded_units(unterminated.as_ptr(), 4) }.is_none());
        // SAFETY: a null pointer is never dereferenced.
        assert!(unsafe { bounded_units(std::ptr::null(), 4) }.is_none());
    }

    #[test]
    fn multi_string_blocks_split_on_single_nuls_and_stop_at_the_double_nul() {
        let block: Vec<u16> = "one\0two\0\0ignored\0".encode_utf16().collect();
        assert_eq!(multi_strings(&block), vec!["one", "two"]);
        assert_eq!(multi_strings(&[0, 0]), Vec::<String>::new());
    }

    #[test]
    fn nul_terminated_buffers_are_truncated_and_wide_null_appends_a_terminator() {
        let mut buffer = vec![0_u16; 8];
        for (slot, unit) in buffer.iter_mut().zip("hi".encode_utf16()) {
            *slot = unit;
        }
        assert_eq!(nul_terminated(&buffer).as_deref(), Some("hi"));
        assert_eq!(wide_null("x").as_slice(), &[b'x' as u16, 0]);
    }
}
