use std::io;
use std::path::Path;
use std::ptr;
use std::thread;
use std::time::{Duration, Instant};

use mactype_service_contract::effective_service_name;
use windows_sys::Win32::Foundation::{
    GetLastError, ERROR_SERVICE_ALREADY_RUNNING, ERROR_SERVICE_MARKED_FOR_DELETE,
    ERROR_SERVICE_NOT_ACTIVE,
};
use windows_sys::Win32::System::Services::{
    ChangeServiceConfigW, ControlService, CreateServiceW, DeleteService, QueryServiceStatusEx,
    StartServiceW, SC_HANDLE, SC_STATUS_PROCESS_INFO, SERVICE_ALL_ACCESS, SERVICE_AUTO_START,
    SERVICE_CHANGE_CONFIG, SERVICE_ERROR_NORMAL, SERVICE_NO_CHANGE, SERVICE_QUERY_CONFIG,
    SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_START, SERVICE_STATUS, SERVICE_STATUS_PROCESS,
    SERVICE_STOP, SERVICE_STOPPED, SERVICE_WIN32_OWN_PROCESS,
};

use super::configuration::{configure_metadata, quoted_image_path, validate_service_binary};
use super::health::wait_for_ready_health;
use super::{wide, ServiceHandle, ServiceManager, DISPLAY_NAME, HEALTH_TIMEOUT, STATE_TIMEOUT};
use crate::SetupError;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AbsenceObservation {
    Absent,
    Present,
    DeletePending,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AbsencePollAction {
    Complete,
    Retry,
}

fn absence_poll_action(
    observation: AbsenceObservation,
    timed_out: bool,
) -> Result<AbsencePollAction, SetupError> {
    if observation == AbsenceObservation::Absent {
        return Ok(AbsencePollAction::Complete);
    }
    if timed_out {
        return Err(SetupError::Runtime(
            "service name remained delete-pending or occupied until removal timeout".to_owned(),
        ));
    }
    Ok(AbsencePollAction::Retry)
}

impl ServiceManager {
    pub fn install(&self, service_binary: &Path) -> Result<(), SetupError> {
        validate_service_binary(&self.protected_root, service_binary)?;
        if self.open_service(SERVICE_QUERY_STATUS)?.is_some() {
            return Err(SetupError::Runtime(
                "the fixed service name already exists; refusing to replace it".to_owned(),
            ));
        }

        let service_name = wide(effective_service_name());
        let display_name = wide(DISPLAY_NAME);
        let image_path = wide(&quoted_image_path(service_binary)?);
        let handle = unsafe {
            CreateServiceW(
                self.handle.0,
                service_name.as_ptr(),
                display_name.as_ptr(),
                SERVICE_ALL_ACCESS,
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL,
                image_path.as_ptr(),
                ptr::null(),
                ptr::null_mut(),
                ptr::null(),
                ptr::null(),
                ptr::null(),
            )
        };
        if handle.is_null() {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        let service = ServiceHandle(handle);
        if let Err(error) = configure_metadata(service.0) {
            unsafe {
                DeleteService(service.0);
            }
            return Err(error);
        }
        Ok(())
    }

    pub fn reconfigure(&self, service_binary: &Path) -> Result<(), SetupError> {
        validate_service_binary(&self.protected_root, service_binary)?;
        let service = self
            .open_service(SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)?
            .ok_or_else(|| SetupError::Runtime("the open service is not installed".to_owned()))?;
        self.ensure_owned(&service)?;
        let image_path = wide(&quoted_image_path(service_binary)?);
        let display_name = wide(DISPLAY_NAME);
        if unsafe {
            ChangeServiceConfigW(
                service.0,
                SERVICE_NO_CHANGE,
                SERVICE_AUTO_START,
                SERVICE_NO_CHANGE,
                image_path.as_ptr(),
                ptr::null(),
                ptr::null_mut(),
                ptr::null(),
                ptr::null(),
                ptr::null(),
                display_name.as_ptr(),
            )
        } == 0
        {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        configure_metadata(service.0)
    }

    pub fn start_and_wait_ready(&self) -> Result<(), SetupError> {
        self.start_and_wait_ready_for(None)
    }

    pub fn start_and_wait_ready_for_profile(
        &self,
        expected_profile_digest: &str,
    ) -> Result<(), SetupError> {
        self.start_and_wait_ready_for(Some(expected_profile_digest))
    }

    fn start_and_wait_ready_for(
        &self,
        expected_profile_digest: Option<&str>,
    ) -> Result<(), SetupError> {
        let service = self
            .open_service(SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)?
            .ok_or_else(|| SetupError::Runtime("the open service is not installed".to_owned()))?;
        self.ensure_owned(&service)?;
        if unsafe { StartServiceW(service.0, 0, ptr::null()) } == 0 {
            let error = unsafe { GetLastError() };
            if error != ERROR_SERVICE_ALREADY_RUNNING {
                return Err(SetupError::Io(io::Error::from_raw_os_error(error as i32)));
            }
        }
        let status = wait_for_state(service.0, SERVICE_RUNNING, STATE_TIMEOUT)?;
        if status.dwProcessId == 0 {
            return Err(SetupError::Runtime(
                "SCM reported a running service without a process identity".to_owned(),
            ));
        }
        wait_for_ready_health(status.dwProcessId, expected_profile_digest, HEALTH_TIMEOUT)
    }

    pub fn is_running(&self) -> Result<bool, SetupError> {
        let service = self
            .open_service(SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)?
            .ok_or_else(|| SetupError::Runtime("the open service is not installed".to_owned()))?;
        self.ensure_owned(&service)?;
        let state = query_status(service.0)?.dwCurrentState;
        match state {
            SERVICE_RUNNING => Ok(true),
            SERVICE_STOPPED => Ok(false),
            _ => Err(SetupError::Runtime(
                "the open service is transitioning and cannot be repaired".to_owned(),
            )),
        }
    }

    pub fn stop(&self) -> Result<(), SetupError> {
        let Some(service) =
            self.open_service(SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)?
        else {
            return Ok(());
        };
        self.ensure_owned(&service)?;
        let current = query_status(service.0)?;
        if current.dwCurrentState == SERVICE_STOPPED {
            return Ok(());
        }
        let mut status = SERVICE_STATUS::default();
        if unsafe { ControlService(service.0, 1, &mut status) } == 0 {
            let error = unsafe { GetLastError() };
            if error != ERROR_SERVICE_NOT_ACTIVE {
                return Err(SetupError::Io(io::Error::from_raw_os_error(error as i32)));
            }
        }
        wait_for_state(service.0, SERVICE_STOPPED, STATE_TIMEOUT).map(|_| ())
    }

    pub fn remove(&self) -> Result<(), SetupError> {
        self.stop()?;
        let Some(service) =
            self.open_service(0x0001_0000 | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)?
        else {
            return Ok(());
        };
        self.ensure_owned(&service)?;
        if unsafe { DeleteService(service.0) } == 0 {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        drop(service);
        self.wait_until_absent(STATE_TIMEOUT)
    }

    fn wait_until_absent(&self, timeout: Duration) -> Result<(), SetupError> {
        let deadline = Instant::now() + timeout;
        loop {
            let observation = match self.open_service(SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG) {
                Ok(None) => AbsenceObservation::Absent,
                Ok(Some(service)) => {
                    self.ensure_owned(&service)?;
                    AbsenceObservation::Present
                }
                Err(error)
                    if error.raw_os_error() == Some(ERROR_SERVICE_MARKED_FOR_DELETE as i32) =>
                {
                    AbsenceObservation::DeletePending
                }
                Err(error) => return Err(SetupError::Io(error)),
            };
            match absence_poll_action(observation, Instant::now() >= deadline)? {
                AbsencePollAction::Complete => return Ok(()),
                AbsencePollAction::Retry => thread::sleep(Duration::from_millis(100)),
            }
        }
    }
}

pub(super) fn query_status(service: SC_HANDLE) -> Result<SERVICE_STATUS_PROCESS, SetupError> {
    let mut status = SERVICE_STATUS_PROCESS::default();
    let mut needed = 0;
    if unsafe {
        QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            (&raw mut status).cast(),
            std::mem::size_of::<SERVICE_STATUS_PROCESS>() as u32,
            &mut needed,
        )
    } == 0
    {
        return Err(SetupError::Io(io::Error::last_os_error()));
    }
    Ok(status)
}

fn wait_for_state(
    service: SC_HANDLE,
    expected: u32,
    timeout: Duration,
) -> Result<SERVICE_STATUS_PROCESS, SetupError> {
    let deadline = Instant::now() + timeout;
    loop {
        let status = query_status(service)?;
        if status.dwCurrentState == expected {
            return Ok(status);
        }
        if status.dwCurrentState == SERVICE_STOPPED && expected != SERVICE_STOPPED {
            return Err(SetupError::Runtime(format!(
                "service stopped before reaching state {expected} (win32={}, service={})",
                status.dwWin32ExitCode, status.dwServiceSpecificExitCode
            )));
        }
        if Instant::now() >= deadline {
            return Err(SetupError::Runtime(format!(
                "service did not reach state {expected} before timeout"
            )));
        }
        thread::sleep(Duration::from_millis(100));
    }
}

#[cfg(test)]
mod tests {
    use super::{absence_poll_action, AbsenceObservation, AbsencePollAction};

    #[test]
    fn service_removal_waits_through_delete_pending_and_times_out_explicitly() {
        assert_eq!(
            absence_poll_action(AbsenceObservation::DeletePending, false).unwrap(),
            AbsencePollAction::Retry
        );
        assert_eq!(
            absence_poll_action(AbsenceObservation::Present, false).unwrap(),
            AbsencePollAction::Retry
        );
        assert_eq!(
            absence_poll_action(AbsenceObservation::Absent, false).unwrap(),
            AbsencePollAction::Complete
        );
        let error = absence_poll_action(AbsenceObservation::DeletePending, true).unwrap_err();
        assert!(error
            .to_string()
            .contains("service name remained delete-pending"));
    }
}
