//! The health named pipe: an outbound message server in the service and a
//! read-only client in the setup broker.

use std::io;
use std::ptr::{null, null_mut};

use windows_sys::Win32::Foundation::{ERROR_NO_DATA, ERROR_PIPE_CONNECTED, ERROR_PIPE_LISTENING};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, ReadFile, WriteFile, FILE_ATTRIBUTE_NORMAL, FILE_FLAG_FIRST_PIPE_INSTANCE,
    FILE_GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, PIPE_ACCESS_OUTBOUND,
};
use windows_sys::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, GetNamedPipeServerProcessId,
    PIPE_NOWAIT, PIPE_READMODE_MESSAGE, PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_MESSAGE,
};

use crate::handle::OwnedHandle;
use crate::security::SecurityDescriptor;
use crate::wide::wide_null;

/// What a non-blocking connect attempt observed.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConnectOutcome {
    /// A client is attached and can be written to.
    Connected,
    /// No client yet; try again later.
    Listening,
}

/// A single-instance, outbound, message-mode pipe server that never blocks
/// and never accepts remote clients.
#[derive(Debug)]
pub struct NamedPipeServer(OwnedHandle);

// SAFETY: the pipe handle is a kernel object; every call on it is serialized
// by the kernel, so the server may be driven from a worker thread.
unsafe impl Send for NamedPipeServer {}

impl NamedPipeServer {
    /// Creates `name` with the given descriptor and an outbound buffer of
    /// `buffer_bytes`.
    pub fn create(
        name: &str,
        descriptor: &SecurityDescriptor,
        buffer_bytes: u32,
    ) -> io::Result<Self> {
        let name = wide_null(name);
        let attributes = descriptor.attributes(false);
        // SAFETY: `name` is NUL-terminated and the attribute block, with the
        // descriptor it points at, outlives the call.
        let handle = unsafe {
            CreateNamedPipeW(
                name.as_ptr(),
                PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE
                    | PIPE_READMODE_MESSAGE
                    | PIPE_NOWAIT
                    | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                buffer_bytes,
                0,
                250,
                attributes.as_ptr(),
            )
        };
        Ok(Self(OwnedHandle::from_creation(handle)?))
    }

    /// Polls for a client. Errors other than "still listening" are returned;
    /// the caller decides whether to disconnect and continue.
    pub fn connect(&self) -> io::Result<ConnectOutcome> {
        // SAFETY: the handle is live; no overlapped structure is passed.
        if unsafe { ConnectNamedPipe(self.0.as_raw(), null_mut()) } != 0 {
            return Ok(ConnectOutcome::Connected);
        }
        let error = io::Error::last_os_error();
        match error.raw_os_error().map(|code| code as u32) {
            Some(ERROR_PIPE_CONNECTED) => Ok(ConnectOutcome::Connected),
            Some(ERROR_PIPE_LISTENING | ERROR_NO_DATA) => Ok(ConnectOutcome::Listening),
            _ => Err(error),
        }
    }

    /// Writes one whole message. A partial write is reported as `WriteZero`.
    pub fn write_message(&self, bytes: &[u8]) -> io::Result<()> {
        let mut written = 0;
        // SAFETY: the handle is live; `bytes` is a live slice whose length is
        // passed alongside its pointer; `written` is a local out value.
        if unsafe {
            WriteFile(
                self.0.as_raw(),
                bytes.as_ptr(),
                bytes.len() as u32,
                &mut written,
                null_mut(),
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        if written as usize != bytes.len() {
            return Err(io::Error::new(
                io::ErrorKind::WriteZero,
                "the pipe accepted only part of a message",
            ));
        }
        Ok(())
    }

    /// Drops the current client, if any, so the next connect can proceed.
    pub fn disconnect(&self) {
        // SAFETY: the handle is live; disconnecting with no client is a
        // harmless failure.
        unsafe { DisconnectNamedPipe(self.0.as_raw()) };
    }
}

impl Drop for NamedPipeServer {
    fn drop(&mut self) {
        self.disconnect();
    }
}

/// A read-only client connection to a named pipe.
#[derive(Debug)]
pub struct NamedPipeClient(OwnedHandle);

impl NamedPipeClient {
    /// Opens `name` for reading. The error carries the Win32 code, so a
    /// caller polling for a server that is not up yet can keep waiting.
    pub fn open_read(name: &str) -> io::Result<Self> {
        let name = wide_null(name);
        // SAFETY: `name` is NUL-terminated; no security attributes or template
        // handle are passed.
        let handle = unsafe {
            CreateFileW(
                name.as_ptr(),
                FILE_GENERIC_READ,
                FILE_SHARE_READ,
                null(),
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                null_mut(),
            )
        };
        Ok(Self(OwnedHandle::from_creation(handle)?))
    }

    /// The PID of the process serving the other end.
    pub fn server_process_id(&self) -> io::Result<u32> {
        let mut pid = 0;
        // SAFETY: the handle is live; the out pointer is a local `u32`.
        if unsafe { GetNamedPipeServerProcessId(self.0.as_raw(), &mut pid) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(pid)
    }

    /// Reads one message into `buffer`, returning the byte count.
    pub fn read(&self, buffer: &mut [u8]) -> io::Result<usize> {
        let mut read = 0;
        // SAFETY: the handle is live; `buffer` is writable for the length
        // passed alongside it; `read` is a local out value.
        if unsafe {
            ReadFile(
                self.0.as_raw(),
                buffer.as_mut_ptr(),
                buffer.len() as u32,
                &mut read,
                null_mut(),
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok((read as usize).min(buffer.len()))
    }
}

#[cfg(test)]
mod tests {
    use std::time::{Duration, Instant};

    use super::{ConnectOutcome, NamedPipeClient, NamedPipeServer};
    use crate::security::SecurityDescriptor;

    #[test]
    fn a_client_reads_the_message_the_server_wrote_and_sees_its_pid() {
        let name = format!(r"\\.\pipe\mactype-platform-{}", std::process::id());
        let descriptor =
            SecurityDescriptor::from_sddl("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)").unwrap();
        let server = NamedPipeServer::create(&name, &descriptor, 4096).unwrap();
        assert_eq!(server.connect().unwrap(), ConnectOutcome::Listening);

        let client = NamedPipeClient::open_read(&name).unwrap();
        assert_eq!(client.server_process_id().unwrap(), std::process::id());
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if server.connect().unwrap() == ConnectOutcome::Connected {
                break;
            }
            assert!(Instant::now() < deadline, "the client never connected");
            std::thread::sleep(Duration::from_millis(5));
        }
        server.write_message(b"hello\n").unwrap();
        let mut buffer = [0_u8; 64];
        let read = client.read(&mut buffer).unwrap();
        assert_eq!(&buffer[..read], b"hello\n");
        server.disconnect();
    }
}
