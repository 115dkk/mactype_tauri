use std::io;
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use mactype_service_contract::HealthReport;
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, LocalFree, ERROR_NO_DATA, ERROR_PIPE_CONNECTED,
    ERROR_PIPE_LISTENING, INVALID_HANDLE_VALUE,
};
use windows_sys::Win32::Security::Authorization::{
    ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
};
use windows_sys::Win32::Security::{PSECURITY_DESCRIPTOR, SECURITY_ATTRIBUTES};
use windows_sys::Win32::Storage::FileSystem::{
    WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_ACCESS_OUTBOUND,
};
use windows_sys::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, PIPE_NOWAIT, PIPE_READMODE_MESSAGE,
    PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_MESSAGE,
};

use crate::HealthPublisher;

pub const HEALTH_PIPE_SECURITY_SDDL: &str = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)";
const MAX_HEALTH_MESSAGE_BYTES: usize = 16 * 1024;

pub struct NamedPipeHealthPublisher {
    current: Arc<Mutex<Vec<u8>>>,
    stop: Arc<AtomicBool>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl NamedPipeHealthPublisher {
    pub fn start(pipe_name: &str) -> io::Result<Self> {
        let pipe_name = wide_null(pipe_name);
        let descriptor = OwnedSecurityDescriptor::from_sddl(HEALTH_PIPE_SECURITY_SDDL)?;
        let security_attributes = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor.0,
            bInheritHandle: 0,
        };
        let handle = unsafe {
            CreateNamedPipeW(
                pipe_name.as_ptr(),
                PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE
                    | PIPE_READMODE_MESSAGE
                    | PIPE_NOWAIT
                    | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                16 * 1024,
                0,
                250,
                &security_attributes,
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }

        let current = Arc::new(Mutex::new(Vec::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let worker_current = Arc::clone(&current);
        let worker_stop = Arc::clone(&stop);
        let raw_handle = handle as isize;
        let worker = thread::spawn(move || {
            serve(raw_handle, worker_current, worker_stop);
        });

        Ok(Self {
            current,
            stop,
            worker: Mutex::new(Some(worker)),
        })
    }
}

struct OwnedSecurityDescriptor(PSECURITY_DESCRIPTOR);

impl OwnedSecurityDescriptor {
    fn from_sddl(sddl: &str) -> io::Result<Self> {
        let sddl = wide_null(sddl);
        let mut descriptor = ptr::null_mut();
        if unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                SDDL_REVISION_1,
                &mut descriptor,
                ptr::null_mut(),
            )
        } == 0
            || descriptor.is_null()
        {
            return Err(io::Error::last_os_error());
        }
        Ok(Self(descriptor))
    }
}

impl Drop for OwnedSecurityDescriptor {
    fn drop(&mut self) {
        unsafe {
            LocalFree(self.0);
        }
    }
}

impl HealthPublisher for NamedPipeHealthPublisher {
    fn publish(&self, report: &HealthReport) -> io::Result<()> {
        report
            .validate()
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
        let mut bytes = serde_json::to_vec(report)?;
        bytes.push(b'\n');
        if bytes.len() > MAX_HEALTH_MESSAGE_BYTES {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "health report exceeds the fixed named-pipe message bound",
            ));
        }
        *self
            .current
            .lock()
            .map_err(|_| io::Error::other("health report lock poisoned"))? = bytes;
        Ok(())
    }
}

impl Drop for NamedPipeHealthPublisher {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.lock().ok().and_then(|mut worker| worker.take()) {
            let _ = worker.join();
        }
    }
}

fn serve(handle: isize, current: Arc<Mutex<Vec<u8>>>, stop: Arc<AtomicBool>) {
    let handle = handle as *mut core::ffi::c_void;
    while !stop.load(Ordering::Acquire) {
        let connected = unsafe { ConnectNamedPipe(handle, ptr::null_mut()) } != 0;
        let error = if connected {
            0
        } else {
            unsafe { GetLastError() }
        };
        if connected || error == ERROR_PIPE_CONNECTED {
            let bytes = match current.lock() {
                Ok(current) => current.clone(),
                Err(poisoned) => poisoned.into_inner().clone(),
            };
            if !bytes.is_empty() {
                let _ = write_message(handle, &bytes);
            }
            unsafe {
                DisconnectNamedPipe(handle);
            }
        } else if !matches!(error, ERROR_PIPE_LISTENING | ERROR_NO_DATA) {
            unsafe {
                DisconnectNamedPipe(handle);
            }
        }
        thread::sleep(Duration::from_millis(25));
    }
    unsafe {
        DisconnectNamedPipe(handle);
        CloseHandle(handle);
    }
}

fn write_message(handle: *mut core::ffi::c_void, bytes: &[u8]) -> io::Result<()> {
    let mut written = 0;
    if unsafe {
        WriteFile(
            handle,
            bytes.as_ptr(),
            bytes.len() as u32,
            &mut written,
            ptr::null_mut(),
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    if written as usize != bytes.len() {
        return Err(io::Error::new(
            io::ErrorKind::WriteZero,
            "health pipe accepted only a partial fixed message",
        ));
    }
    Ok(())
}

fn wide_null(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}
