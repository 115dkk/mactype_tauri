mod security;

use std::io;
use std::time::Duration;

use windows_sys::Win32::Foundation::{
    CloseHandle, ERROR_ACCESS_DENIED, WAIT_ABANDONED, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows_sys::Win32::Security::SECURITY_ATTRIBUTES;
use windows_sys::Win32::System::Threading::{CreateMutexW, ReleaseMutex, WaitForSingleObject};

use crate::SetupError;
use security::OwnedSecurityDescriptor;

const SETUP_MUTEX_NAME: &str = "Global\\MacTypeControlCenter.Setup.v1";
const SETUP_MUTEX_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Debug)]
pub(super) struct MachineSetupLock {
    handle: windows_sys::Win32::Foundation::HANDLE,
    _abandoned: bool,
}

impl MachineSetupLock {
    pub(super) fn acquire() -> Result<Self, SetupError> {
        Self::acquire_with_timeout(SETUP_MUTEX_TIMEOUT)
    }

    #[cfg(test)]
    pub(super) fn acquire_for_test(timeout: Duration) -> Result<Self, SetupError> {
        Self::acquire_with_timeout(timeout)
    }

    #[cfg(test)]
    fn acquire_named_for_test(name: &str, timeout: Duration) -> Result<Self, SetupError> {
        Self::acquire_named_with_timeout(name, timeout)
    }

    fn acquire_with_timeout(timeout: Duration) -> Result<Self, SetupError> {
        Self::acquire_named_with_timeout(SETUP_MUTEX_NAME, timeout)
    }

    fn acquire_named_with_timeout(name: &str, timeout: Duration) -> Result<Self, SetupError> {
        let descriptor = OwnedSecurityDescriptor::for_machine_setup_lock()?;
        let security = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor.as_ptr(),
            bInheritHandle: 0,
        };
        let name = wide(name);
        let handle = unsafe { CreateMutexW(&security, 0, name.as_ptr()) };
        if handle.is_null() {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(ERROR_ACCESS_DENIED as i32) {
                return Err(foreign_lock_error(
                    "the named object cannot be opened for security verification",
                ));
            }
            return Err(SetupError::Io(error));
        }
        if let Err(error) = security::verify_machine_setup_lock(handle) {
            unsafe {
                CloseHandle(handle);
            }
            return Err(error);
        }
        let timeout_ms = timeout.as_millis().min(u128::from(u32::MAX - 1)) as u32;
        let wait = unsafe { WaitForSingleObject(handle, timeout_ms) };
        match wait {
            WAIT_OBJECT_0 | WAIT_ABANDONED => Ok(Self {
                handle,
                _abandoned: wait == WAIT_ABANDONED,
            }),
            WAIT_TIMEOUT => {
                unsafe {
                    CloseHandle(handle);
                }
                Err(SetupError::Runtime(
                    "another machine setup operation did not finish before the bounded wait expired"
                        .to_owned(),
                ))
            }
            _ => {
                let error = io::Error::last_os_error();
                unsafe {
                    CloseHandle(handle);
                }
                Err(SetupError::Io(error))
            }
        }
    }
}

impl Drop for MachineSetupLock {
    fn drop(&mut self) {
        unsafe {
            ReleaseMutex(self.handle);
            CloseHandle(self.handle);
        }
    }
}

fn foreign_lock_error(detail: &str) -> SetupError {
    SetupError::Runtime(format!("foreign machine setup lock rejected: {detail}"))
}

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static TEST_MUTEX_ID: AtomicU64 = AtomicU64::new(0);

    fn unique_mutex_name(label: &str) -> String {
        format!(
            "Local\\MacTypeControlCenter.Setup.test.{}.{}.{}",
            std::process::id(),
            TEST_MUTEX_ID.fetch_add(1, Ordering::Relaxed),
            label
        )
    }

    #[test]
    fn a_permissive_precreated_mutex_is_rejected_as_foreign() {
        let name = unique_mutex_name("foreign");
        let descriptor = OwnedSecurityDescriptor::from_sddl_for_test("D:P(A;;GA;;;AU)").unwrap();
        let security = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor.as_ptr(),
            bInheritHandle: 0,
        };
        let wide_name = wide(&name);
        let foreign = unsafe { CreateMutexW(&security, 0, wide_name.as_ptr()) };
        assert!(
            !foreign.is_null(),
            "failed to create the foreign test mutex"
        );

        let error =
            MachineSetupLock::acquire_named_for_test(&name, Duration::from_millis(25)).unwrap_err();
        assert!(
            error.to_string().contains("foreign machine setup lock"),
            "unexpected lock error: {error}"
        );

        unsafe {
            CloseHandle(foreign);
        }
    }
}
