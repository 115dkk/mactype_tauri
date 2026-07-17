use super::{
    super::{
        decode_profile_transfer_frame, ProfileTransferToken, PROFILE_TRANSFER_HEADER_BYTES,
        PROFILE_TRANSFER_MAGIC, PROFILE_TRANSFER_VERSION,
    },
    shared::*,
};
use std::{
    fs,
    os::windows::{fs::OpenOptionsExt, io::AsRawHandle},
    thread,
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{GetLastError, ERROR_FILE_NOT_FOUND, ERROR_IO_PENDING, ERROR_PIPE_BUSY, HANDLE},
    Storage::FileSystem::{
        ReadFile, FILE_FLAG_OVERLAPPED, SECURITY_IDENTIFICATION, SECURITY_SQOS_PRESENT,
    },
    System::{Pipes::GetNamedPipeServerProcessId, IO::OVERLAPPED},
};

pub(in crate::machine_integration::open_service) fn receive_profile_from_pipe_bounded(
    token: &ProfileTransferToken,
    timeout: Duration,
) -> Result<Vec<u8>, String> {
    if token.server_pid == 0 || timeout.is_zero() {
        return Err("the profile pipe token or timeout is invalid".to_owned());
    }
    let deadline = Instant::now() + timeout;
    let pipe_name = profile_pipe_name(token);
    let pipe = loop {
        let mut options = fs::OpenOptions::new();
        options
            .read(true)
            .custom_flags(FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION);
        match options.open(&pipe_name) {
            Ok(pipe) => break pipe,
            Err(error)
                if matches!(
                    error.raw_os_error().map(|value| value as u32),
                    Some(ERROR_FILE_NOT_FOUND) | Some(ERROR_PIPE_BUSY)
                ) =>
            {
                if Instant::now() >= deadline {
                    return Err("connecting to the profile pipe timed out".to_owned());
                }
                thread::sleep(PROFILE_PIPE_POLL);
            }
            Err(error) => return Err(error.to_string()),
        }
    };
    let mut actual_server_pid = 0;
    if unsafe { GetNamedPipeServerProcessId(pipe.as_raw_handle(), &mut actual_server_pid) } == 0 {
        return Err(format!(
            "querying the profile pipe server failed with {}",
            unsafe { GetLastError() }
        ));
    }
    if actual_server_pid != token.server_pid {
        return Err("the profile pipe server PID does not match the broker token".to_owned());
    }
    let maximum = PROFILE_TRANSFER_HEADER_BYTES + mactype_service_contract::MAX_PROFILE_BYTES;
    let mut frame = Vec::with_capacity(PROFILE_TRANSFER_HEADER_BYTES);
    let mut expected_total = None;
    loop {
        let remaining = expected_total
            .map(|expected| expected - frame.len())
            .unwrap_or(maximum - frame.len());
        if remaining == 0 {
            return Err("the profile pipe frame exceeds the fixed size limit".to_owned());
        }
        let mut chunk = vec![0_u8; remaining.min(64 * 1024)];
        let read = read_overlapped_pipe_chunk(pipe.as_raw_handle(), &mut chunk, deadline)?;
        if read == 0 {
            return Err("the profile pipe closed before sending a complete frame".to_owned());
        }
        frame.extend_from_slice(&chunk[..read]);
        if expected_total.is_none() && frame.len() >= 12 {
            if &frame[..4] != PROFILE_TRANSFER_MAGIC
                || u16::from_le_bytes(frame[4..6].try_into().expect("fixed frame version prefix"))
                    != PROFILE_TRANSFER_VERSION
                || u16::from_le_bytes(frame[6..8].try_into().expect("fixed reserved prefix")) != 0
            {
                return Err("profile transfer frame header is invalid".to_owned());
            }
            let payload_len =
                u32::from_le_bytes(frame[8..12].try_into().expect("fixed frame length prefix"))
                    as usize;
            if payload_len == 0 || payload_len > mactype_service_contract::MAX_PROFILE_BYTES {
                return Err("profile transfer frame length is invalid".to_owned());
            }
            expected_total = Some(PROFILE_TRANSFER_HEADER_BYTES + payload_len);
        }
        if expected_total.is_some_and(|expected| frame.len() > expected) {
            return Err("profile transfer frame has trailing bytes".to_owned());
        }
        if expected_total == Some(frame.len()) {
            return decode_profile_transfer_frame(&frame, &token.nonce);
        }
    }
}

fn read_overlapped_pipe_chunk(
    pipe: HANDLE,
    buffer: &mut [u8],
    deadline: Instant,
) -> Result<usize, String> {
    let event = create_profile_pipe_event("read")?;
    let mut overlapped = OVERLAPPED {
        hEvent: event.raw(),
        ..Default::default()
    };
    let mut transferred = 0;
    if unsafe {
        ReadFile(
            pipe,
            buffer.as_mut_ptr(),
            buffer.len() as u32,
            &mut transferred,
            &mut overlapped,
        )
    } == 0
    {
        let error = unsafe { GetLastError() };
        if error != ERROR_IO_PENDING {
            return Err(format!("reading the profile pipe failed with {error}"));
        }
        transferred = wait_for_profile_pipe_operation(
            pipe,
            &overlapped,
            deadline,
            None,
            "receiving the profile pipe frame",
        )?;
    }
    Ok(transferred as usize)
}
