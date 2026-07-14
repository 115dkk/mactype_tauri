use std::{env, fs::OpenOptions, io::Write, path::PathBuf};

const READY_MARKER_ENV: &str = "MACTYPE_CI_SINGLE_INSTANCE_READY";
const EVENT_MARKER_ENV: &str = "MACTYPE_CI_SINGLE_INSTANCE_EVENTS";

#[cfg(windows)]
const STARTUP_GATE_NAME: &str = "Local\\MacTypeControlCenter.StartupGate";

pub(crate) struct StartupGate {
    #[cfg(windows)]
    handle: isize,
}

impl StartupGate {
    #[cfg(windows)]
    pub(crate) fn acquire() -> Result<Self, String> {
        use std::{iter, ptr};
        use windows_sys::Win32::{
            Foundation::{GetLastError, WAIT_ABANDONED, WAIT_OBJECT_0, WAIT_TIMEOUT},
            System::Threading::{CreateMutexW, WaitForSingleObject},
        };

        let name = STARTUP_GATE_NAME
            .encode_utf16()
            .chain(iter::once(0))
            .collect::<Vec<_>>();
        let handle = unsafe { CreateMutexW(ptr::null(), 0, name.as_ptr()) };
        if handle.is_null() {
            return Err(format!(
                "failed to create the single-instance startup gate: Windows error {}",
                unsafe { GetLastError() }
            ));
        }

        let wait_result = unsafe { WaitForSingleObject(handle, 30_000) };
        if wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED {
            unsafe {
                windows_sys::Win32::Foundation::CloseHandle(handle);
            }
            let reason = if wait_result == WAIT_TIMEOUT {
                "timed out after 30 seconds".to_owned()
            } else {
                format!("failed with wait result 0x{wait_result:08x}")
            };
            return Err(format!(
                "failed to acquire the single-instance startup gate: {reason}"
            ));
        }

        Ok(Self {
            handle: handle as isize,
        })
    }

    #[cfg(not(windows))]
    pub(crate) fn acquire() -> Result<Self, String> {
        Ok(Self {})
    }

    #[cfg(windows)]
    pub(crate) fn release(mut self) -> Result<(), String> {
        use windows_sys::Win32::{
            Foundation::{CloseHandle, GetLastError},
            System::Threading::ReleaseMutex,
        };

        let handle = self.handle as _;
        self.handle = 0;
        let released = unsafe { ReleaseMutex(handle) };
        let release_error = if released == 0 {
            Some(unsafe { GetLastError() })
        } else {
            None
        };
        unsafe {
            CloseHandle(handle);
        }
        if let Some(error) = release_error {
            return Err(format!(
                "failed to release the single-instance startup gate: Windows error {error}"
            ));
        }
        Ok(())
    }

    #[cfg(not(windows))]
    pub(crate) fn release(self) -> Result<(), String> {
        Ok(())
    }
}

#[cfg(windows)]
impl Drop for StartupGate {
    fn drop(&mut self) {
        if self.handle == 0 {
            return;
        }
        let handle = self.handle as _;
        unsafe {
            windows_sys::Win32::System::Threading::ReleaseMutex(handle);
            windows_sys::Win32::Foundation::CloseHandle(handle);
        }
        self.handle = 0;
    }
}

pub(crate) fn write_ready_marker() -> Result<(), String> {
    let Some(path) = env::var_os(READY_MARKER_ENV) else {
        return Ok(());
    };
    std::fs::write(PathBuf::from(path), format!("{}\n", std::process::id()))
        .map_err(|error| format!("failed to write single-instance ready marker: {error}"))
}

pub(crate) fn record_activation(
    args: Vec<String>,
    cwd: String,
    restored: bool,
) -> Result<(), String> {
    let Some(path) = env::var_os(EVENT_MARKER_ENV) else {
        return Ok(());
    };
    let event = serde_json::json!({
        "args": args,
        "cwd": cwd,
        "restored": restored,
        "pid": std::process::id(),
    });
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(PathBuf::from(path))
        .map_err(|error| format!("failed to open single-instance event marker: {error}"))?;
    writeln!(file, "{event}")
        .map_err(|error| format!("failed to write single-instance event marker: {error}"))
}
