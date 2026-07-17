use std::ffi::c_void;
use std::io;
use std::ptr;
use std::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};
use std::time::Duration;

use mactype_service_contract::StructuredServiceError;
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_SUCCESS, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows_sys::Win32::System::RemoteDesktop::WTSSESSION_NOTIFICATION;
use windows_sys::Win32::System::Services::{
    RegisterServiceCtrlHandlerExW, SetServiceStatus, SERVICE_RUNNING, SERVICE_START_PENDING,
    SERVICE_STATUS, SERVICE_STATUS_HANDLE, SERVICE_STOPPED, SERVICE_STOP_PENDING,
    SERVICE_WIN32_OWN_PROCESS,
};
use windows_sys::Win32::System::Threading::{CreateEventW, SetEvent, WaitForSingleObject};

use crate::session_event_queue::SessionEventQueue;
use crate::{
    ScmState, ServiceControl, ServiceStatus, SessionChange, StatusReporter, StopSignal,
    ACCEPTED_CONTROL_MASK, SERVICE_STOP_WAIT_HINT_MS,
};

static STATUS_HANDLE: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());
static STOP_EVENT: AtomicUsize = AtomicUsize::new(0);
static SESSION_CHANGES: SessionEventQueue = SessionEventQueue::new();

pub(super) struct ServiceControlContext {
    stop_event: *mut c_void,
}

impl ServiceControlContext {
    pub(super) fn register(service_name: &[u16]) -> io::Result<Self> {
        let status_handle = unsafe {
            RegisterServiceCtrlHandlerExW(service_name.as_ptr(), Some(control_handler), ptr::null())
        };
        if status_handle.is_null() {
            return Err(io::Error::last_os_error());
        }
        STATUS_HANDLE.store(status_handle, Ordering::Release);

        let stop_event = unsafe { CreateEventW(ptr::null(), 1, 0, ptr::null()) };
        if stop_event.is_null() {
            let error = io::Error::last_os_error();
            let reporter = Win32StatusReporter;
            let _ = reporter.report(ServiceStatus::stopped_with_error(
                error.raw_os_error().unwrap_or(1) as u32,
                0,
            ));
            STATUS_HANDLE.store(ptr::null_mut(), Ordering::Release);
            return Err(error);
        }
        STOP_EVENT.store(stop_event as usize, Ordering::Release);
        Ok(Self { stop_event })
    }
}

impl Drop for ServiceControlContext {
    fn drop(&mut self) {
        STOP_EVENT.store(0, Ordering::Release);
        STATUS_HANDLE.store(ptr::null_mut(), Ordering::Release);
        unsafe { CloseHandle(self.stop_event) };
    }
}

unsafe extern "system" fn control_handler(
    control: u32,
    event_type: u32,
    event_data: *mut c_void,
    _context: *mut c_void,
) -> u32 {
    match ServiceControl::from_raw(control, event_type) {
        Some(ServiceControl::Stop | ServiceControl::Shutdown) => {
            let reporter = Win32StatusReporter;
            let _ = reporter.report(ServiceStatus::stop_pending(1, SERVICE_STOP_WAIT_HINT_MS));
            let event = STOP_EVENT.load(Ordering::Acquire);
            if event != 0 {
                unsafe { SetEvent(event as *mut c_void) };
            }
        }
        Some(ServiceControl::SessionChange { .. }) if !event_data.is_null() => {
            let notification = unsafe { &*(event_data.cast::<WTSSESSION_NOTIFICATION>()) };
            if notification.cbSize as usize >= std::mem::size_of::<WTSSESSION_NOTIFICATION>() {
                SESSION_CHANGES.push(event_type, notification.dwSessionId);
            }
        }
        Some(ServiceControl::SessionChange { .. }) | None => {}
    }
    ERROR_SUCCESS
}

pub(super) struct Win32StatusReporter;

impl StatusReporter for Win32StatusReporter {
    fn report(&self, status: ServiceStatus) -> io::Result<()> {
        let handle: SERVICE_STATUS_HANDLE = STATUS_HANDLE.load(Ordering::Acquire);
        if handle.is_null() {
            return Err(io::Error::new(
                io::ErrorKind::NotConnected,
                "SCM status handler is not registered",
            ));
        }
        let current_state = match status.state {
            ScmState::StartPending => SERVICE_START_PENDING,
            ScmState::Running => SERVICE_RUNNING,
            ScmState::StopPending => SERVICE_STOP_PENDING,
            ScmState::Stopped => SERVICE_STOPPED,
        };
        let native = SERVICE_STATUS {
            dwServiceType: SERVICE_WIN32_OWN_PROCESS,
            dwCurrentState: current_state,
            dwControlsAccepted: if status.state == ScmState::Running {
                ACCEPTED_CONTROL_MASK
            } else {
                0
            },
            dwWin32ExitCode: status.win32_exit_code,
            dwServiceSpecificExitCode: status.service_specific_exit_code,
            dwCheckPoint: status.checkpoint,
            dwWaitHint: status.wait_hint_ms,
        };
        if unsafe { SetServiceStatus(handle, &native) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }
}

pub(super) struct Win32StopSignal;

impl StopSignal for Win32StopSignal {
    fn wait(&self) -> Result<(), StructuredServiceError> {
        let event = stop_event()?;
        let result = unsafe { WaitForSingleObject(event, u32::MAX) };
        if result != WAIT_OBJECT_0 {
            return Err(stop_wait_error());
        }
        Ok(())
    }

    fn wait_timeout(&self, timeout: Duration) -> Result<bool, StructuredServiceError> {
        let event = stop_event()?;
        let timeout_ms = timeout.as_millis().min(u128::from(u32::MAX - 1)) as u32;
        match unsafe { WaitForSingleObject(event, timeout_ms) } {
            WAIT_OBJECT_0 => Ok(true),
            WAIT_TIMEOUT => Ok(false),
            _ => Err(stop_wait_error()),
        }
    }

    fn take_session_change(&self) -> Option<SessionChange> {
        SESSION_CHANGES.pop()
    }
}

fn stop_event() -> Result<*mut c_void, StructuredServiceError> {
    let event = STOP_EVENT.load(Ordering::Acquire);
    if event == 0 {
        Err(StructuredServiceError {
            code: "stop-event-unavailable".to_owned(),
            message: "service stop event was not initialized".to_owned(),
            win32_error: None,
        })
    } else {
        Ok(event as *mut c_void)
    }
}

fn stop_wait_error() -> StructuredServiceError {
    StructuredServiceError {
        code: "stop-wait-failed".to_owned(),
        message: "waiting for the service stop event failed".to_owned(),
        win32_error: Some(unsafe { GetLastError() }),
    }
}

pub(crate) fn stop_requested() -> bool {
    let event = STOP_EVENT.load(Ordering::Acquire);
    event != 0 && unsafe { WaitForSingleObject(event as *mut c_void, 0) } == WAIT_OBJECT_0
}
