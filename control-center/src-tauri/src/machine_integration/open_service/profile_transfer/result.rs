use super::{
    super::{
        decode_broker_result_frame, encode_broker_result_frame, BrokerResultMessage,
        ProfileTransferToken, BROKER_RESULT_HEADER_BYTES, BROKER_RESULT_MAGIC,
        BROKER_RESULT_VERSION, MAX_BROKER_RESULT_BYTES, PROFILE_TRANSFER_NONCE_BYTES,
    },
    handle::OwnedKernelHandle,
    shared::*,
};
use std::{
    fs,
    os::windows::{fs::OpenOptionsExt, io::AsRawHandle},
    ptr, thread,
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{
        GetLastError, LocalFree, ERROR_FILE_NOT_FOUND, ERROR_IO_PENDING, ERROR_PIPE_BUSY,
        ERROR_PIPE_CONNECTED, HANDLE, INVALID_HANDLE_VALUE,
    },
    Security::{
        Authorization::{ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1},
        SECURITY_ATTRIBUTES,
    },
    Storage::FileSystem::{
        ReadFile, WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED,
        PIPE_ACCESS_INBOUND, SECURITY_IDENTIFICATION, SECURITY_SQOS_PRESENT,
    },
    System::{
        Pipes::{
            ConnectNamedPipe, CreateNamedPipeW, GetNamedPipeClientProcessId,
            GetNamedPipeServerProcessId, PIPE_READMODE_BYTE, PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_TYPE_BYTE,
        },
        IO::OVERLAPPED,
    },
};

pub(in crate::machine_integration::open_service) struct BrokerResultPipeServer {
    handle: OwnedKernelHandle,
    token: ProfileTransferToken,
}

impl BrokerResultPipeServer {
    pub(in crate::machine_integration::open_service) fn create() -> Result<Self, String> {
        Self::create_with_nonce(random_profile_transfer_nonce()?)
    }

    pub(in crate::machine_integration::open_service) fn create_with_nonce(
        nonce: [u8; PROFILE_TRANSFER_NONCE_BYTES],
    ) -> Result<Self, String> {
        let token = ProfileTransferToken {
            server_pid: std::process::id(),
            nonce,
        };
        let descriptor_text = wide(PROFILE_PIPE_SDDL);
        let mut descriptor = ptr::null_mut();
        if unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                descriptor_text.as_ptr(),
                SDDL_REVISION_1,
                &mut descriptor,
                ptr::null_mut(),
            )
        } == 0
        {
            return Err(format!(
                "creating the local broker result pipe ACL failed with {}",
                unsafe { GetLastError() }
            ));
        }
        let attributes = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor,
            bInheritHandle: 0,
        };
        let name = wide(result_pipe_name(&token));
        let handle = unsafe {
            CreateNamedPipeW(
                name.as_ptr(),
                PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                0,
                (BROKER_RESULT_HEADER_BYTES + MAX_BROKER_RESULT_BYTES) as u32,
                PROFILE_PIPE_TIMEOUT.as_millis() as u32,
                &attributes,
            )
        };
        unsafe { LocalFree(descriptor) };
        if handle == INVALID_HANDLE_VALUE {
            return Err(format!(
                "creating the first broker result pipe instance failed with {}",
                unsafe { GetLastError() }
            ));
        }
        Ok(Self {
            handle: OwnedKernelHandle(handle),
            token,
        })
    }

    pub(in crate::machine_integration::open_service) fn token(&self) -> &ProfileTransferToken {
        &self.token
    }

    pub(in crate::machine_integration::open_service) fn receive_from(
        self,
        expected_client_pid: u32,
        broker_process: Option<HANDLE>,
        timeout: Duration,
    ) -> Result<BrokerResultMessage, String> {
        if expected_client_pid == 0 || timeout.is_zero() {
            return Err("the broker result pipe peer or timeout is invalid".to_owned());
        }
        let deadline = Instant::now() + timeout;
        let connect_event = create_profile_pipe_event("result connect")?;
        let mut connect_overlapped = OVERLAPPED {
            hEvent: connect_event.raw(),
            ..Default::default()
        };
        if unsafe { ConnectNamedPipe(self.handle.raw(), &mut connect_overlapped) } == 0 {
            let error = unsafe { GetLastError() };
            if error == ERROR_IO_PENDING {
                wait_for_profile_pipe_operation(
                    self.handle.raw(),
                    &connect_overlapped,
                    deadline,
                    broker_process,
                    "waiting for the broker result pipe client",
                )?;
            } else if error != ERROR_PIPE_CONNECTED {
                return Err(format!(
                    "connecting the broker result pipe failed with {error}"
                ));
            }
        }
        let mut actual_client_pid = 0;
        if unsafe { GetNamedPipeClientProcessId(self.handle.raw(), &mut actual_client_pid) } == 0 {
            return Err(format!(
                "querying the broker result pipe client failed with {}",
                unsafe { GetLastError() }
            ));
        }
        if actual_client_pid != expected_client_pid {
            return Err(
                "the first broker result pipe client is not the elevated broker".to_owned(),
            );
        }

        let maximum = BROKER_RESULT_HEADER_BYTES + MAX_BROKER_RESULT_BYTES;
        let mut frame = Vec::with_capacity(BROKER_RESULT_HEADER_BYTES);
        let mut expected_total = None;
        loop {
            let remaining = expected_total
                .map(|expected| expected - frame.len())
                .unwrap_or(maximum - frame.len());
            if remaining == 0 {
                return Err("the broker result frame exceeds the fixed size limit".to_owned());
            }
            let mut chunk = vec![0_u8; remaining.min(16 * 1024)];
            let read = read_chunk(self.handle.raw(), &mut chunk, deadline, broker_process)?;
            if read == 0 {
                return Err(
                    "the broker result pipe closed before sending a complete frame".to_owned(),
                );
            }
            frame.extend_from_slice(&chunk[..read]);
            if expected_total.is_none() && frame.len() >= 12 {
                if &frame[..4] != BROKER_RESULT_MAGIC
                    || u16::from_le_bytes(frame[4..6].try_into().expect("fixed result version"))
                        != BROKER_RESULT_VERSION
                    || u16::from_le_bytes(frame[6..8].try_into().expect("fixed result reserved"))
                        != 0
                {
                    return Err("broker result frame header is invalid".to_owned());
                }
                let payload_len =
                    u32::from_le_bytes(frame[8..12].try_into().expect("fixed result length"))
                        as usize;
                if payload_len == 0 || payload_len > MAX_BROKER_RESULT_BYTES {
                    return Err("broker result frame length is invalid".to_owned());
                }
                expected_total = Some(BROKER_RESULT_HEADER_BYTES + payload_len);
            }
            if expected_total.is_some_and(|expected| frame.len() > expected) {
                return Err("broker result frame has trailing bytes".to_owned());
            }
            if expected_total == Some(frame.len()) {
                return decode_broker_result_frame(&frame, &self.token.nonce);
            }
        }
    }
}

pub(in crate::machine_integration::open_service) struct BrokerResultPipeWriter {
    pipe: fs::File,
    nonce: [u8; PROFILE_TRANSFER_NONCE_BYTES],
}

impl BrokerResultPipeWriter {
    pub(in crate::machine_integration::open_service) fn connect(
        token: &ProfileTransferToken,
        timeout: Duration,
    ) -> Result<Self, String> {
        if token.server_pid == 0 || timeout.is_zero() {
            return Err("the broker result pipe token or timeout is invalid".to_owned());
        }
        let deadline = Instant::now() + timeout;
        let name = result_pipe_name(token);
        let pipe = loop {
            let mut options = fs::OpenOptions::new();
            options.write(true).custom_flags(
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            );
            match options.open(&name) {
                Ok(pipe) => break pipe,
                Err(error)
                    if matches!(
                        error.raw_os_error().map(|value| value as u32),
                        Some(ERROR_FILE_NOT_FOUND) | Some(ERROR_PIPE_BUSY)
                    ) =>
                {
                    if Instant::now() >= deadline {
                        return Err("connecting to the broker result pipe timed out".to_owned());
                    }
                    thread::sleep(PROFILE_PIPE_POLL);
                }
                Err(error) => return Err(error.to_string()),
            }
        };
        let mut actual_server_pid = 0;
        if unsafe { GetNamedPipeServerProcessId(pipe.as_raw_handle(), &mut actual_server_pid) } == 0
        {
            return Err(format!(
                "querying the broker result pipe server failed with {}",
                unsafe { GetLastError() }
            ));
        }
        if actual_server_pid != token.server_pid {
            return Err(
                "the broker result pipe server PID does not match the broker token".to_owned(),
            );
        }
        Ok(Self {
            pipe,
            nonce: token.nonce,
        })
    }

    pub(in crate::machine_integration::open_service) fn send(
        self,
        message: &BrokerResultMessage,
        timeout: Duration,
    ) -> Result<(), String> {
        if timeout.is_zero() {
            return Err("the broker result pipe timeout is invalid".to_owned());
        }
        let frame = encode_broker_result_frame(message, &self.nonce)?;
        let deadline = Instant::now() + timeout;
        let mut written = 0;
        while written < frame.len() {
            let event = create_profile_pipe_event("result write")?;
            let mut overlapped = OVERLAPPED {
                hEvent: event.raw(),
                ..Default::default()
            };
            let remaining = &frame[written..];
            let mut transferred = 0;
            if unsafe {
                WriteFile(
                    self.pipe.as_raw_handle(),
                    remaining.as_ptr(),
                    remaining.len() as u32,
                    &mut transferred,
                    &mut overlapped,
                )
            } == 0
            {
                let error = unsafe { GetLastError() };
                if error != ERROR_IO_PENDING {
                    return Err(format!(
                        "writing the broker result pipe failed with {error}"
                    ));
                }
                transferred = wait_for_profile_pipe_operation(
                    self.pipe.as_raw_handle(),
                    &overlapped,
                    deadline,
                    None,
                    "sending the broker result frame",
                )?;
            }
            if transferred == 0 {
                return Err("writing the broker result pipe made no progress".to_owned());
            }
            written += transferred as usize;
        }
        Ok(())
    }
}

fn read_chunk(
    pipe: HANDLE,
    buffer: &mut [u8],
    deadline: Instant,
    broker_process: Option<HANDLE>,
) -> Result<usize, String> {
    let event = create_profile_pipe_event("result read")?;
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
            return Err(format!(
                "reading the broker result pipe failed with {error}"
            ));
        }
        transferred = wait_for_profile_pipe_operation(
            pipe,
            &overlapped,
            deadline,
            broker_process,
            "receiving the broker result frame",
        )?;
    }
    Ok(transferred as usize)
}

fn result_pipe_name(token: &ProfileTransferToken) -> String {
    format!(
        r"\\.\pipe\MacTypeControlCenter.broker-result.v1.{}.{}",
        token.server_pid,
        profile_transfer_nonce_text(&token.nonce)
    )
}
