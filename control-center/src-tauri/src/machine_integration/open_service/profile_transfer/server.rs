use super::{
    super::{encode_profile_transfer_frame, ProfileTransferToken, PROFILE_TRANSFER_NONCE_BYTES},
    handle::OwnedKernelHandle,
    shared::*,
};
use std::{
    ptr,
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{
        GetLastError, LocalFree, ERROR_IO_PENDING, ERROR_PIPE_CONNECTED, HANDLE,
        INVALID_HANDLE_VALUE,
    },
    Security::{
        Authorization::{ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1},
        SECURITY_ATTRIBUTES,
    },
    Storage::FileSystem::{
        WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED, PIPE_ACCESS_OUTBOUND,
    },
    System::{
        Pipes::{
            ConnectNamedPipe, CreateNamedPipeW, GetNamedPipeClientProcessId, PIPE_READMODE_BYTE,
            PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE,
        },
        IO::OVERLAPPED,
    },
};

pub(in crate::machine_integration::open_service) struct ProfilePipeServer {
    handle: OwnedKernelHandle,
    #[cfg(test)]
    token: ProfileTransferToken,
    frame: Vec<u8>,
}

impl ProfilePipeServer {
    #[cfg(test)]
    pub(in crate::machine_integration::open_service) fn create(
        profile: &[u8],
    ) -> Result<Self, String> {
        Self::create_with_nonce(profile, random_profile_transfer_nonce()?)
    }

    pub(in crate::machine_integration::open_service) fn create_with_nonce(
        profile: &[u8],
        nonce: [u8; PROFILE_TRANSFER_NONCE_BYTES],
    ) -> Result<Self, String> {
        let token = ProfileTransferToken {
            server_pid: std::process::id(),
            nonce,
        };
        let frame = encode_profile_transfer_frame(profile, &token.nonce)?;
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
                "creating the local profile pipe ACL failed with {}",
                unsafe { GetLastError() }
            ));
        }
        let attributes = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor,
            bInheritHandle: 0,
        };
        let name = wide(profile_pipe_name(&token));
        let handle = unsafe {
            CreateNamedPipeW(
                name.as_ptr(),
                PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                (frame.len() as u32).min(PROFILE_PIPE_BUFFER_BYTES),
                0,
                PROFILE_PIPE_TIMEOUT.as_millis() as u32,
                &attributes,
            )
        };
        unsafe { LocalFree(descriptor) };
        if handle == INVALID_HANDLE_VALUE {
            return Err(format!(
                "creating the first profile pipe instance failed with {}",
                unsafe { GetLastError() }
            ));
        }
        Ok(Self {
            handle: OwnedKernelHandle(handle),
            #[cfg(test)]
            token,
            frame,
        })
    }

    #[cfg(test)]
    pub(in crate::machine_integration::open_service) fn token(&self) -> &ProfileTransferToken {
        &self.token
    }

    pub(in crate::machine_integration::open_service) fn send_to(
        self,
        expected_client_pid: u32,
        broker_process: Option<HANDLE>,
        timeout: Duration,
    ) -> Result<(), String> {
        if expected_client_pid == 0 || timeout.is_zero() {
            return Err("the profile pipe peer or timeout is invalid".to_owned());
        }
        let deadline = Instant::now() + timeout;
        let connect_event = create_profile_pipe_event("connect")?;
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
                    "waiting for the profile pipe client",
                )?;
            } else if error != ERROR_PIPE_CONNECTED {
                return Err(format!("connecting the profile pipe failed with {error}"));
            }
        }

        let mut actual_client_pid = 0;
        if unsafe { GetNamedPipeClientProcessId(self.handle.raw(), &mut actual_client_pid) } == 0 {
            return Err(format!(
                "querying the profile pipe client failed with {}",
                unsafe { GetLastError() }
            ));
        }
        if actual_client_pid != expected_client_pid {
            return Err("the first profile pipe client is not the elevated broker".to_owned());
        }
        let mut written = 0;
        while written < self.frame.len() {
            let event = create_profile_pipe_event("write")?;
            let mut overlapped = OVERLAPPED {
                hEvent: event.raw(),
                ..Default::default()
            };
            let remaining = &self.frame[written..];
            let mut transferred = 0;
            if unsafe {
                WriteFile(
                    self.handle.raw(),
                    remaining.as_ptr(),
                    remaining.len() as u32,
                    &mut transferred,
                    &mut overlapped,
                )
            } == 0
            {
                let error = unsafe { GetLastError() };
                if error != ERROR_IO_PENDING {
                    return Err(format!("writing the profile pipe failed with {error}"));
                }
                transferred = wait_for_profile_pipe_operation(
                    self.handle.raw(),
                    &overlapped,
                    deadline,
                    broker_process,
                    "sending the profile pipe frame",
                )?;
            }
            if transferred == 0 {
                return Err("writing the profile pipe made no progress".to_owned());
            }
            written += transferred as usize;
        }
        Ok(())
    }
}
