use super::{
    super::{ProfileTransferToken, PROFILE_TRANSFER_NONCE_BYTES},
    handle::OwnedKernelHandle,
};
use std::{
    ptr,
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{GetLastError, ERROR_NOT_FOUND, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT},
    Security::Cryptography::{BCryptGenRandom, BCRYPT_USE_SYSTEM_PREFERRED_RNG},
    System::{
        Threading::{CreateEventW, WaitForSingleObject},
        IO::{CancelIoEx, GetOverlappedResult, GetOverlappedResultEx, OVERLAPPED},
    },
};

pub(super) fn wide(value: impl AsRef<std::ffi::OsStr>) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    value.as_ref().encode_wide().chain(Some(0)).collect()
}
pub(in crate::machine_integration::open_service) const PROFILE_PIPE_TIMEOUT: Duration =
    Duration::from_secs(60);
pub(super) const PROFILE_PIPE_POLL: Duration = Duration::from_millis(10);
pub(super) const PROFILE_PIPE_BUFFER_BYTES: u32 = 64 * 1024;
pub(in crate::machine_integration::open_service) const PROFILE_PIPE_SDDL: &str =
    "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)(A;;GR;;;AU)";

#[cfg(test)]
thread_local! {
    static PROFILE_PIPE_REAP_COUNT: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
}

#[cfg(test)]
pub(in crate::machine_integration::open_service) fn reset_profile_pipe_reap_count() {
    PROFILE_PIPE_REAP_COUNT.set(0);
}

#[cfg(test)]
pub(in crate::machine_integration::open_service) fn profile_pipe_reap_count() -> usize {
    PROFILE_PIPE_REAP_COUNT.get()
}

pub(super) fn create_profile_pipe_event(operation: &str) -> Result<OwnedKernelHandle, String> {
    let event = unsafe { CreateEventW(ptr::null(), 1, 0, ptr::null()) };
    if event.is_null() {
        Err(format!(
            "creating the profile pipe {operation} event failed with {}",
            unsafe { GetLastError() }
        ))
    } else {
        Ok(OwnedKernelHandle(event))
    }
}

pub(super) fn wait_for_profile_pipe_operation(
    pipe: HANDLE,
    overlapped: &OVERLAPPED,
    deadline: Instant,
    broker_process: Option<HANDLE>,
    operation: &str,
) -> Result<u32, String> {
    loop {
        if let Some(process) = broker_process {
            let state = unsafe { WaitForSingleObject(process, 0) };
            if state == WAIT_OBJECT_0 {
                cancel_and_reap_profile_pipe_operation(pipe, overlapped)?;
                return Err(format!("the elevated broker exited while {operation}"));
            }
            if state != WAIT_TIMEOUT {
                cancel_and_reap_profile_pipe_operation(pipe, overlapped)?;
                return Err(format!(
                    "checking the elevated broker while {operation} failed with {state}"
                ));
            }
        }
        let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
            cancel_and_reap_profile_pipe_operation(pipe, overlapped)?;
            return Err(format!("{operation} timed out"));
        };
        let wait = remaining.min(PROFILE_PIPE_POLL);
        let wait_ms = wait.as_millis().clamp(1, u32::MAX as u128) as u32;
        let mut transferred = 0;
        if unsafe { GetOverlappedResultEx(pipe, overlapped, &mut transferred, wait_ms, 0) } != 0 {
            return Ok(transferred);
        }
        let error = unsafe { GetLastError() };
        if error != WAIT_TIMEOUT {
            cancel_and_reap_profile_pipe_operation(pipe, overlapped)?;
            return Err(format!("{operation} failed with {error}"));
        }
    }
}

pub(super) fn cancel_and_reap_profile_pipe_operation(
    pipe: HANDLE,
    overlapped: &OVERLAPPED,
) -> Result<(), String> {
    #[cfg(test)]
    PROFILE_PIPE_REAP_COUNT.set(PROFILE_PIPE_REAP_COUNT.get() + 1);
    let cancel_error = if unsafe { CancelIoEx(pipe, overlapped) } == 0 {
        Some(unsafe { GetLastError() })
    } else {
        None
    };
    let mut transferred = 0;
    unsafe {
        GetOverlappedResult(pipe, overlapped, &mut transferred, 1);
    }
    match cancel_error {
        Some(error) if error != ERROR_NOT_FOUND => Err(format!(
            "cancelling the profile pipe operation failed with {error}"
        )),
        _ => Ok(()),
    }
}

pub(in crate::machine_integration::open_service) fn random_profile_transfer_nonce(
) -> Result<[u8; PROFILE_TRANSFER_NONCE_BYTES], String> {
    let mut nonce = [0_u8; PROFILE_TRANSFER_NONCE_BYTES];
    let status = unsafe {
        BCryptGenRandom(
            ptr::null_mut(),
            nonce.as_mut_ptr(),
            nonce.len() as u32,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG,
        )
    };
    if status < 0 {
        Err(format!(
            "generating the profile transfer nonce failed with NTSTATUS {status:#x}"
        ))
    } else {
        Ok(nonce)
    }
}

pub(in crate::machine_integration::open_service) fn profile_transfer_nonce_text(
    nonce: &[u8; PROFILE_TRANSFER_NONCE_BYTES],
) -> String {
    let mut text = String::with_capacity(PROFILE_TRANSFER_NONCE_BYTES * 2);
    for byte in nonce {
        use std::fmt::Write as _;
        write!(text, "{byte:02x}").expect("writing to a String cannot fail");
    }
    text
}

pub(super) fn profile_pipe_name(token: &ProfileTransferToken) -> String {
    format!(
        r"\\.\pipe\MacTypeControlCenter.profile.v1.{}.{}",
        token.server_pid,
        profile_transfer_nonce_text(&token.nonce)
    )
}
