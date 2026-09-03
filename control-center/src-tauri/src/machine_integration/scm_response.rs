//! An SCM query response held in word-aligned storage, with every embedded
//! pointer checked against the allocation before it is read.

use std::mem::{align_of, size_of};
use windows_sys::Win32::Foundation::{ERROR_INSUFFICIENT_BUFFER, ERROR_INVALID_DATA};

/// The largest response any SCM query may hand back (QueryServiceConfigW
/// documents 8 KiB; Config2 levels and security descriptors are larger).
pub(crate) const MAX_SCM_RESPONSE_BYTES: u32 = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum ScmResponseError {
    /// The size probe or the fill call failed with this Win32 code.
    Win32(u32),
    /// The reported size was outside `minimum..=MAX_SCM_RESPONSE_BYTES`, or
    /// the response is shorter than its fixed header.
    Size,
    /// A pointer lies outside the response or is misaligned for its type.
    OutOfResponse,
    /// A string has no NUL inside the response (or a MULTI_SZ has no
    /// terminating empty string inside it).
    Unterminated,
    /// A string is not valid UTF-16 (only from the strict decoder).
    InvalidUtf16,
    /// More entries than the caller's cap.
    TooManyEntries,
}

impl ScmResponseError {
    /// The Win32 code callers with a `u32` error channel report: the
    /// original code for `Win32`, `ERROR_INVALID_DATA` for every malformed
    /// response.
    pub(crate) fn win32_code(self) -> u32 {
        match self {
            Self::Win32(code) => code,
            Self::Size
            | Self::OutOfResponse
            | Self::Unterminated
            | Self::InvalidUtf16
            | Self::TooManyEntries => ERROR_INVALID_DATA,
        }
    }

    /// A short description for callers with a `String` error channel.
    pub(crate) fn describe(self) -> &'static str {
        match self {
            Self::Win32(_) => "the Win32 call failed",
            Self::Size => "the response size is invalid",
            Self::OutOfResponse => "the response contains an out-of-range pointer",
            Self::Unterminated => "a response string is not terminated",
            Self::InvalidUtf16 => "a response string contains invalid UTF-16",
            Self::TooManyEntries => "the response contains too many entries",
        }
    }
}

pub(crate) struct ScmResponse {
    words: Vec<usize>,
    byte_length: usize,
}

impl ScmResponse {
    /// Runs the two-call size-probe-then-fill protocol. `call(buffer,
    /// capacity, needed)` performs exactly one Win32 call with those
    /// arguments and returns its BOOL. First invocation: null buffer, zero
    /// capacity; must return 0 with ERROR_INSUFFICIENT_BUFFER and a size in
    /// `minimum_bytes..=MAX_SCM_RESPONSE_BYTES`. Second: the word-aligned
    /// buffer and its FULL allocation capacity (`size_of_val(words)`).
    /// `byte_length = min(reported, capacity)`, must be >= minimum_bytes.
    pub(crate) fn query(
        minimum_bytes: usize,
        mut call: impl FnMut(*mut u8, u32, &mut u32) -> i32,
    ) -> Result<Self, ScmResponseError> {
        if minimum_bytes > MAX_SCM_RESPONSE_BYTES as usize {
            return Err(ScmResponseError::Size);
        }

        let mut needed = 0;
        let initial = call(std::ptr::null_mut(), 0, &mut needed);
        let initial_error = std::io::Error::last_os_error()
            .raw_os_error()
            .map_or(ERROR_INVALID_DATA, |code| code as u32);
        if initial != 0 || initial_error != ERROR_INSUFFICIENT_BUFFER {
            return Err(ScmResponseError::Win32(initial_error));
        }
        if needed < minimum_bytes as u32 || needed > MAX_SCM_RESPONSE_BYTES {
            return Err(ScmResponseError::Size);
        }

        let word_size = size_of::<usize>();
        let mut words = vec![0usize; (needed as usize).div_ceil(word_size)];
        let capacity = std::mem::size_of_val(words.as_slice()) as u32;
        let filled = call(words.as_mut_ptr().cast(), capacity, &mut needed);
        let fill_error = std::io::Error::last_os_error()
            .raw_os_error()
            .map_or(ERROR_INVALID_DATA, |code| code as u32);
        if filled == 0 {
            return Err(ScmResponseError::Win32(fill_error));
        }

        let byte_length = (needed as usize).min(capacity as usize);
        if byte_length < minimum_bytes {
            return Err(ScmResponseError::Size);
        }
        Ok(Self { words, byte_length })
    }

    /// The fixed header at the start of the response (requires
    /// `byte_length >= size_of::<T>()`; T's alignment must not exceed
    /// `align_of::<usize>()` — assert it).
    pub(crate) fn header<T: Copy>(&self) -> Result<T, ScmResponseError> {
        assert!(align_of::<T>() <= align_of::<usize>());
        if self.byte_length < size_of::<T>() {
            return Err(ScmResponseError::Size);
        }
        // SAFETY: `words` is aligned for `T`, and the byte-length check proves
        // that the complete fixed header lies inside the response.
        Ok(unsafe { self.words.as_ptr().cast::<T>().read() })
    }

    /// NUL-terminated UTF-16 that `pointer` addresses inside this response;
    /// `Ok(None)` for null. Checks address in `[start, end)` and
    /// `address % align_of::<u16>() == 0`; the NUL must be inside the
    /// response.
    pub(crate) fn wide_units(
        &self,
        pointer: *const u16,
    ) -> Result<Option<&[u16]>, ScmResponseError> {
        if pointer.is_null() {
            return Ok(None);
        }
        let offset = self.checked_offset(pointer.cast(), align_of::<u16>())?;
        let available = (self.byte_length - offset) / size_of::<u16>();
        // SAFETY: `checked_offset` proves that `offset` is inside `words` and
        // aligned for `u16`; `available` keeps the slice inside `byte_length`.
        let units = unsafe {
            std::slice::from_raw_parts(
                self.words.as_ptr().cast::<u8>().add(offset).cast::<u16>(),
                available,
            )
        };
        let length = units
            .iter()
            .position(|unit| *unit == 0)
            .ok_or(ScmResponseError::Unterminated)?;
        Ok(Some(&units[..length]))
    }

    /// Lossy string form of `wide_units` (empty string for null).
    pub(crate) fn wide_string_lossy(
        &self,
        pointer: *const u16,
    ) -> Result<String, ScmResponseError> {
        Ok(self
            .wide_units(pointer)?
            .map_or_else(String::new, String::from_utf16_lossy))
    }

    /// Strict string form of `wide_units` (`None` for null; `InvalidUtf16` on
    /// bad units). This is what `snapshot.rs` needs to keep its exact
    /// round-trip semantics.
    pub(crate) fn wide_string(
        &self,
        pointer: *const u16,
    ) -> Result<Option<String>, ScmResponseError> {
        self.wide_units(pointer)?
            .map(|units| String::from_utf16(units).map_err(|_| ScmResponseError::InvalidUtf16))
            .transpose()
    }

    /// Double-NUL-terminated block, ended by the first empty string which
    /// must itself be inside the response. `Ok(empty)` for null. Errors with
    /// `TooManyEntries` past `max_entries`.
    pub(crate) fn multi_units(
        &self,
        pointer: *const u16,
        max_entries: usize,
    ) -> Result<Vec<&[u16]>, ScmResponseError> {
        if pointer.is_null() {
            return Ok(Vec::new());
        }
        let offset = self.checked_offset(pointer.cast(), align_of::<u16>())?;
        let available = (self.byte_length - offset) / size_of::<u16>();
        // SAFETY: `checked_offset` proves that `offset` is inside `words` and
        // aligned for `u16`; `available` keeps the slice inside `byte_length`.
        let units = unsafe {
            std::slice::from_raw_parts(
                self.words.as_ptr().cast::<u8>().add(offset).cast::<u16>(),
                available,
            )
        };

        let mut entries = Vec::new();
        let mut current = 0;
        while current < units.len() {
            let relative_end = units[current..]
                .iter()
                .position(|unit| *unit == 0)
                .ok_or(ScmResponseError::Unterminated)?;
            let end = current + relative_end;
            if end == current {
                return Ok(entries);
            }
            if entries.len() >= max_entries {
                return Err(ScmResponseError::TooManyEntries);
            }
            entries.push(&units[current..end]);
            current = end + 1;
        }
        Err(ScmResponseError::Unterminated)
    }

    /// `count` elements of `T` at `pointer` inside this response (checked_mul
    /// / checked_add for the byte range; alignment of `T`; null or
    /// `count > max_entries` is an error unless count == 0).
    pub(crate) fn array<T: Copy>(
        &self,
        pointer: *const T,
        count: u32,
        max_entries: usize,
    ) -> Result<Vec<T>, ScmResponseError> {
        let count = count as usize;
        if count == 0 {
            return Ok(Vec::new());
        }
        if count > max_entries {
            return Err(ScmResponseError::TooManyEntries);
        }
        if pointer.is_null() {
            return Err(ScmResponseError::OutOfResponse);
        }

        let offset = self.checked_offset(pointer.cast(), align_of::<T>())?;
        let byte_length = count
            .checked_mul(size_of::<T>())
            .ok_or(ScmResponseError::OutOfResponse)?;
        let range_end = offset
            .checked_add(byte_length)
            .ok_or(ScmResponseError::OutOfResponse)?;
        if range_end > self.byte_length {
            return Err(ScmResponseError::OutOfResponse);
        }
        // SAFETY: the offset and alignment checks validate the first element,
        // and the checked byte range proves that all `count` elements fit.
        let values = unsafe {
            std::slice::from_raw_parts(
                self.words.as_ptr().cast::<u8>().add(offset).cast::<T>(),
                count,
            )
        };
        Ok(values.to_vec())
    }

    fn checked_offset(
        &self,
        pointer: *const u8,
        alignment: usize,
    ) -> Result<usize, ScmResponseError> {
        let start = self.words.as_ptr() as usize;
        let end = start
            .checked_add(self.byte_length)
            .ok_or(ScmResponseError::OutOfResponse)?;
        let address = pointer as usize;
        if address < start || address >= end || address % alignment != 0 {
            return Err(ScmResponseError::OutOfResponse);
        }
        Ok(address - start)
    }

    #[cfg(test)]
    pub(crate) fn from_words(words: Vec<usize>, byte_length: usize) -> Self {
        assert!(byte_length <= std::mem::size_of_val(words.as_slice()));
        Self { words, byte_length }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn response_with_units(
        offset_units: usize,
        units: &[u16],
        trailing_units: usize,
    ) -> (ScmResponse, *const u16) {
        let total_units = offset_units + units.len() + trailing_units;
        let word_size_units = size_of::<usize>() / size_of::<u16>();
        let mut words = vec![0usize; total_units.div_ceil(word_size_units)];
        let storage_units = std::mem::size_of_val(words.as_slice()) / size_of::<u16>();
        // SAFETY: the word allocation contains `storage_units` complete u16
        // values and has alignment at least that of `u16`.
        let storage = unsafe {
            std::slice::from_raw_parts_mut(words.as_mut_ptr().cast::<u16>(), storage_units)
        };
        storage[offset_units..offset_units + units.len()].copy_from_slice(units);
        let pointer = words.as_ptr().cast::<u16>().wrapping_add(offset_units);
        let response = ScmResponse::from_words(words, total_units * size_of::<u16>());
        (response, pointer)
    }

    #[test]
    fn reads_terminated_string_lossy_and_strict() {
        let units: Vec<u16> = "hello".encode_utf16().chain(Some(0)).collect();
        let (response, pointer) = response_with_units(2, &units, 1);

        assert_eq!(response.wide_string_lossy(pointer).unwrap(), "hello");
        assert_eq!(
            response.wide_string(pointer).unwrap(),
            Some("hello".to_owned())
        );
    }

    #[test]
    fn rejects_nul_past_byte_length() {
        let units: Vec<u16> = "hello".encode_utf16().chain(Some(0)).collect();
        let (mut response, pointer) = response_with_units(0, &units, 0);
        response.byte_length -= size_of::<u16>();

        assert_eq!(
            response.wide_units(pointer),
            Err(ScmResponseError::Unterminated)
        );
    }

    #[test]
    fn rejects_out_of_response_and_odd_string_pointers() {
        let units: Vec<u16> = "x".encode_utf16().chain(Some(0)).collect();
        let (response, pointer) = response_with_units(1, &units, 1);
        let start = response.words.as_ptr().cast::<u16>();
        let end = response
            .words
            .as_ptr()
            .cast::<u8>()
            .wrapping_add(response.byte_length)
            .cast::<u16>();
        let odd = pointer.cast::<u8>().wrapping_add(1).cast::<u16>();

        assert_eq!(
            response.wide_units(start.wrapping_sub(1)),
            Err(ScmResponseError::OutOfResponse)
        );
        assert_eq!(
            response.wide_units(end),
            Err(ScmResponseError::OutOfResponse)
        );
        assert_eq!(
            response.wide_units(end.wrapping_add(1)),
            Err(ScmResponseError::OutOfResponse)
        );
        assert_eq!(
            response.wide_units(odd),
            Err(ScmResponseError::OutOfResponse)
        );
    }

    #[test]
    fn reads_multi_string_and_empty_multi_string() {
        let units: Vec<u16> = "one\0two\0\0".encode_utf16().collect();
        let (response, pointer) = response_with_units(0, &units, 0);
        let entries = response.multi_units(pointer, 2).unwrap();
        assert_eq!(entries.len(), 2);
        assert_eq!(String::from_utf16(entries[0]).unwrap(), "one");
        assert_eq!(String::from_utf16(entries[1]).unwrap(), "two");

        let (empty, pointer) = response_with_units(0, &[0], 0);
        assert!(empty.multi_units(pointer, 0).unwrap().is_empty());
    }

    #[test]
    fn rejects_multi_string_without_empty_terminator() {
        let units: Vec<u16> = "one\0two\0".encode_utf16().collect();
        let (response, pointer) = response_with_units(0, &units, 0);

        assert_eq!(
            response.multi_units(pointer, 2),
            Err(ScmResponseError::Unterminated)
        );
    }

    #[test]
    fn enforces_multi_string_entry_cap() {
        let units: Vec<u16> = "one\0two\0\0".encode_utf16().collect();
        let (response, pointer) = response_with_units(0, &units, 0);

        assert_eq!(
            response.multi_units(pointer, 1),
            Err(ScmResponseError::TooManyEntries)
        );
    }

    #[test]
    fn rejects_short_header() {
        let response = ScmResponse::from_words(vec![0usize; 1], 3);

        assert_eq!(response.header::<u32>(), Err(ScmResponseError::Size));
    }

    #[test]
    fn array_rejects_out_of_response_range_and_misalignment() {
        let response = ScmResponse::from_words(vec![0usize; 2], 2 * size_of::<usize>());
        let start = response.words.as_ptr().cast::<u32>();
        let last = response
            .words
            .as_ptr()
            .cast::<u8>()
            .wrapping_add(response.byte_length - size_of::<u32>())
            .cast::<u32>();
        let misaligned = start.cast::<u8>().wrapping_add(1).cast::<u32>();

        assert_eq!(
            response.array(last, 2, 2),
            Err(ScmResponseError::OutOfResponse)
        );
        assert_eq!(
            response.array(misaligned, 1, 1),
            Err(ScmResponseError::OutOfResponse)
        );
    }
}
