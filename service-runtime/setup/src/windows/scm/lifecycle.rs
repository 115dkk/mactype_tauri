use std::io;
use std::path::Path;
use std::ptr;
use std::thread;
use std::time::{Duration, Instant};

use mactype_service_contract::{effective_service_name, HealthReport, HealthState};
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_INVALID_PARAMETER, ERROR_SERVICE_ALREADY_RUNNING,
    ERROR_SERVICE_MARKED_FOR_DELETE, ERROR_SERVICE_NOT_ACTIVE, HANDLE, WAIT_FAILED, WAIT_OBJECT_0,
    WAIT_TIMEOUT,
};
use windows_sys::Win32::Storage::FileSystem::SYNCHRONIZE;
use windows_sys::Win32::System::Services::{
    ChangeServiceConfigW, ControlService, CreateServiceW, DeleteService, QueryServiceStatusEx,
    StartServiceW, SC_HANDLE, SC_STATUS_PROCESS_INFO, SERVICE_ALL_ACCESS, SERVICE_AUTO_START,
    SERVICE_CHANGE_CONFIG, SERVICE_ERROR_NORMAL, SERVICE_NO_CHANGE, SERVICE_QUERY_CONFIG,
    SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_START, SERVICE_STATUS, SERVICE_STATUS_PROCESS,
    SERVICE_STOP, SERVICE_STOPPED, SERVICE_WIN32_OWN_PROCESS,
};
use windows_sys::Win32::System::Threading::{OpenProcess, WaitForSingleObject};

use super::configuration::{configure_metadata, quoted_image_path, validate_service_binary};
use super::health::wait_for_ready_health;
use super::{wide, ServiceHandle, ServiceManager, DISPLAY_NAME, HEALTH_TIMEOUT, STATE_TIMEOUT};
use crate::storage::read_bounded_regular_file;
use crate::SetupError;

const MAX_PERSISTED_HEALTH_BYTES: u64 = 16 * 1024;
const RECONFIGURE_ACCESS: u32 =
    SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_START;

struct ProcessExitHandle(HANDLE);

impl ProcessExitHandle {
    fn open(pid: u32) -> io::Result<Self> {
        let handle = unsafe { OpenProcess(SYNCHRONIZE, 0, pid) };
        if handle.is_null() {
            Err(io::Error::last_os_error())
        } else {
            Ok(Self(handle))
        }
    }
}

impl Drop for ProcessExitHandle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}

fn wait_for_process_exit(process: &ProcessExitHandle, deadline: Instant) -> Result<(), SetupError> {
    let remaining = deadline.saturating_duration_since(Instant::now());
    let timeout_ms = remaining.as_millis().min(u128::from(u32::MAX - 1)) as u32;
    let timeout_ms = if remaining.is_zero() {
        0
    } else {
        timeout_ms.max(1)
    };
    match unsafe { WaitForSingleObject(process.0, timeout_ms) } {
        WAIT_OBJECT_0 => Ok(()),
        WAIT_TIMEOUT => Err(SetupError::Runtime(
            "service process did not exit before the stop timeout".to_owned(),
        )),
        WAIT_FAILED => Err(SetupError::Io(io::Error::last_os_error())),
        result => Err(SetupError::Runtime(format!(
            "service process exit wait returned unknown result {result}"
        ))),
    }
}

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ProcessCaptureTarget {
    AlreadyStopped,
    Pid(u32),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ProcessIdentityObservation {
    Stopped,
    Same,
    Changed(u32),
}

fn process_capture_target(
    status: &SERVICE_STATUS_PROCESS,
) -> Result<ProcessCaptureTarget, SetupError> {
    if status.dwCurrentState == SERVICE_STOPPED {
        return Ok(ProcessCaptureTarget::AlreadyStopped);
    }
    if status.dwProcessId == 0 {
        return Err(SetupError::Runtime(
            "SCM reported a non-stopped service without a process identity".to_owned(),
        ));
    }
    Ok(ProcessCaptureTarget::Pid(status.dwProcessId))
}

fn observe_process_identity(
    captured_pid: u32,
    status: &SERVICE_STATUS_PROCESS,
) -> Result<ProcessIdentityObservation, SetupError> {
    match process_capture_target(status)? {
        ProcessCaptureTarget::AlreadyStopped => Ok(ProcessIdentityObservation::Stopped),
        ProcessCaptureTarget::Pid(observed_pid) if observed_pid == captured_pid => {
            Ok(ProcessIdentityObservation::Same)
        }
        ProcessCaptureTarget::Pid(observed_pid) => {
            Ok(ProcessIdentityObservation::Changed(observed_pid))
        }
    }
}

fn stopping_process_is_complete(
    captured_pid: u32,
    status: &SERVICE_STATUS_PROCESS,
) -> Result<bool, SetupError> {
    match observe_process_identity(captured_pid, status)? {
        ProcessIdentityObservation::Stopped => Ok(true),
        ProcessIdentityObservation::Same => Ok(false),
        ProcessIdentityObservation::Changed(observed_pid) => Err(SetupError::Runtime(format!(
            "service process identity changed while stopping ({captured_pid} -> {observed_pid})"
        ))),
    }
}

fn capture_service_process(
    service: SC_HANDLE,
    deadline: Instant,
) -> Result<Option<(u32, ProcessExitHandle)>, SetupError> {
    loop {
        let status = query_status(service)?;
        let pid = match process_capture_target(&status)? {
            ProcessCaptureTarget::AlreadyStopped => return Ok(None),
            ProcessCaptureTarget::Pid(pid) => pid,
        };

        match ProcessExitHandle::open(pid) {
            Ok(process) => match observe_process_identity(pid, &query_status(service)?)? {
                ProcessIdentityObservation::Stopped | ProcessIdentityObservation::Same => {
                    return Ok(Some((pid, process)))
                }
                ProcessIdentityObservation::Changed(_) => {
                    wait_before_process_capture_retry(deadline)?
                }
            },
            Err(error) if error.raw_os_error() == Some(ERROR_INVALID_PARAMETER as i32) => {
                match observe_process_identity(pid, &query_status(service)?)? {
                    ProcessIdentityObservation::Stopped => return Ok(None),
                    ProcessIdentityObservation::Same | ProcessIdentityObservation::Changed(_) => {
                        wait_before_process_capture_retry(deadline)?
                    }
                }
            }
            Err(error) => {
                return Err(SetupError::Runtime(format!(
                    "could not capture the service process identity before stopping: {error}"
                )))
            }
        }
    }
}

fn wait_before_process_capture_retry(deadline: Instant) -> Result<(), SetupError> {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return Err(SetupError::Runtime(
            "service process identity could not be captured before the stop timeout".to_owned(),
        ));
    }
    thread::sleep(remaining.min(Duration::from_millis(10)));
    Ok(())
}

fn wait_for_stopped_process(
    service: SC_HANDLE,
    captured_pid: u32,
    deadline: Instant,
) -> Result<(), SetupError> {
    loop {
        let status = query_status(service)?;
        if stopping_process_is_complete(captured_pid, &status)? {
            return Ok(());
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(SetupError::Runtime(
                "service did not reach the stopped state before timeout".to_owned(),
            ));
        }
        thread::sleep(remaining.min(Duration::from_millis(100)));
    }
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
            .open_service(RECONFIGURE_ACCESS)?
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
        configure_metadata(service.0).map_err(|error| {
            error.at_machine_path("configure service recovery metadata", service_binary)
        })
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
        let health_path = self.protected_root.join("health.json");
        let status = wait_for_state(
            service.0,
            SERVICE_RUNNING,
            STATE_TIMEOUT,
            Some(&health_path),
        )?;
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
        let deadline = Instant::now() + STATE_TIMEOUT;
        let Some((captured_pid, process)) = capture_service_process(service.0, deadline)? else {
            return Ok(());
        };
        let mut status = SERVICE_STATUS::default();
        if unsafe { ControlService(service.0, 1, &mut status) } == 0 {
            let error = unsafe { GetLastError() };
            if error != ERROR_SERVICE_NOT_ACTIVE {
                return Err(SetupError::Io(io::Error::from_raw_os_error(error as i32)));
            }
        }
        wait_for_stopped_process(service.0, captured_pid, deadline)?;
        wait_for_process_exit(&process, deadline)
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
    failure_health_path: Option<&Path>,
) -> Result<SERVICE_STATUS_PROCESS, SetupError> {
    let deadline = Instant::now() + timeout;
    loop {
        let status = query_status(service)?;
        if status.dwCurrentState == expected {
            return Ok(status);
        }
        if status.dwCurrentState == SERVICE_STOPPED && expected != SERVICE_STOPPED {
            return Err(stopped_before_expected_state_error(
                &status,
                expected,
                failure_health_path,
            ));
        }
        if Instant::now() >= deadline {
            return Err(SetupError::Runtime(format!(
                "service did not reach state {expected} before timeout"
            )));
        }
        thread::sleep(Duration::from_millis(100));
    }
}

fn stopped_before_expected_state_error(
    status: &SERVICE_STATUS_PROCESS,
    expected: u32,
    failure_health_path: Option<&Path>,
) -> SetupError {
    let mut message = format!(
        "service stopped before reaching state {expected} (win32={}, service={})",
        status.dwWin32ExitCode, status.dwServiceSpecificExitCode
    );
    if let Some(diagnostic) = failure_health_path.and_then(persisted_failure_diagnostic) {
        message.push_str("; persisted health failure: ");
        message.push_str(&diagnostic);
    }
    SetupError::Runtime(message)
}

fn persisted_failure_diagnostic(path: &Path) -> Option<String> {
    let bytes = read_bounded_regular_file(
        path,
        MAX_PERSISTED_HEALTH_BYTES,
        "persisted service health diagnostic",
    )
    .ok()?;
    let report: HealthReport = serde_json::from_slice(&bytes).ok()?;
    report.validate().ok()?;
    if report.health != HealthState::Failed {
        return None;
    }
    let error = report.last_error?;
    Some(match error.win32_error {
        Some(code) => format!("{}: {} (win32={code})", error.code, error.message),
        None => format!("{}: {}", error.code, error.message),
    })
}

#[cfg(test)]
mod tests {
    use std::process::Command;
    use std::time::{Duration, Instant};

    use mactype_service_contract::{
        HealthReport, HealthState, InjectionTelemetry, ReadinessReport, StructuredServiceError,
        HEALTH_PROTOCOL_VERSION,
    };
    use windows_sys::Win32::System::Services::{
        SERVICE_RUNNING, SERVICE_START, SERVICE_STATUS_PROCESS, SERVICE_STOPPED,
    };

    use super::{
        absence_poll_action, observe_process_identity, process_capture_target,
        stopped_before_expected_state_error, stopping_process_is_complete, wait_for_process_exit,
        AbsenceObservation, AbsencePollAction, ProcessExitHandle, ProcessIdentityObservation,
        RECONFIGURE_ACCESS,
    };

    const PROCESS_EXIT_CHILD_ENV: &str = "MACTYPE_SETUP_PROCESS_EXIT_CHILD";

    #[test]
    fn process_exit_wait_child() {
        if std::env::var_os(PROCESS_EXIT_CHILD_ENV).is_some() {
            std::thread::sleep(Duration::from_millis(250));
        }
    }

    #[test]
    fn process_exit_wait_tracks_the_captured_process_until_it_signals() {
        let started = Instant::now();
        let mut child = Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "windows::scm::lifecycle::tests::process_exit_wait_child",
            ])
            .env(PROCESS_EXIT_CHILD_ENV, "1")
            .spawn()
            .unwrap();
        let process = ProcessExitHandle::open(child.id()).unwrap();

        wait_for_process_exit(&process, Instant::now() + Duration::from_secs(5)).unwrap();

        assert!(child.wait().unwrap().success());
        assert!(
            started.elapsed() >= Duration::from_millis(100),
            "the wait returned before the captured child exited"
        );
    }

    #[test]
    fn non_stopped_service_without_a_pid_is_rejected_as_unknown_identity() {
        let status = SERVICE_STATUS_PROCESS {
            dwCurrentState: SERVICE_RUNNING,
            dwProcessId: 0,
            ..SERVICE_STATUS_PROCESS::default()
        };

        let error = process_capture_target(&status).unwrap_err();

        assert!(error.to_string().contains("without a process identity"));
    }

    #[test]
    fn process_that_exits_before_capture_is_safe_only_after_scm_reports_stopped() {
        let stopped = SERVICE_STATUS_PROCESS {
            dwCurrentState: SERVICE_STOPPED,
            ..SERVICE_STATUS_PROCESS::default()
        };
        let still_running = SERVICE_STATUS_PROCESS {
            dwCurrentState: SERVICE_RUNNING,
            dwProcessId: 42,
            ..SERVICE_STATUS_PROCESS::default()
        };

        assert_eq!(
            observe_process_identity(42, &stopped).unwrap(),
            ProcessIdentityObservation::Stopped
        );
        assert_eq!(
            observe_process_identity(42, &still_running).unwrap(),
            ProcessIdentityObservation::Same
        );
    }

    #[test]
    fn captured_process_identity_is_discarded_if_scm_changes_pid() {
        let same_process = SERVICE_STATUS_PROCESS {
            dwCurrentState: SERVICE_RUNNING,
            dwProcessId: 41,
            ..SERVICE_STATUS_PROCESS::default()
        };
        let replaced_process = SERVICE_STATUS_PROCESS {
            dwCurrentState: SERVICE_RUNNING,
            dwProcessId: 42,
            ..SERVICE_STATUS_PROCESS::default()
        };
        let stopped = SERVICE_STATUS_PROCESS {
            dwCurrentState: SERVICE_STOPPED,
            ..SERVICE_STATUS_PROCESS::default()
        };

        assert_eq!(
            observe_process_identity(41, &same_process).unwrap(),
            ProcessIdentityObservation::Same
        );
        assert_eq!(
            observe_process_identity(41, &replaced_process).unwrap(),
            ProcessIdentityObservation::Changed(42)
        );
        assert_eq!(
            observe_process_identity(41, &stopped).unwrap(),
            ProcessIdentityObservation::Stopped
        );
    }

    #[test]
    fn service_process_identity_change_after_stop_request_fails_closed() {
        let replacement = SERVICE_STATUS_PROCESS {
            dwCurrentState: SERVICE_RUNNING,
            dwProcessId: 42,
            ..SERVICE_STATUS_PROCESS::default()
        };

        let error = stopping_process_is_complete(41, &replacement).unwrap_err();

        assert!(error
            .to_string()
            .contains("process identity changed while stopping"));
    }

    #[test]
    fn service_reconfiguration_can_apply_restart_recovery_metadata() {
        assert_ne!(
            RECONFIGURE_ACCESS & SERVICE_START,
            0,
            "SC_ACTION_RESTART metadata requires a service handle with SERVICE_START"
        );
    }

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

    #[test]
    fn stopped_service_diagnostic_includes_the_bounded_persisted_failure() {
        let directory = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
        let health_path = directory.path().join("health.json");
        let failure = HealthReport {
            protocol_version: HEALTH_PROTOCOL_VERSION,
            service_version: "0.2.0".to_owned(),
            health: HealthState::Failed,
            active_profile_digest: None,
            readiness: ReadinessReport::initializing(),
            injection: InjectionTelemetry::default(),
            last_error: Some(StructuredServiceError {
                code: "activation-recovery-required".to_owned(),
                message: "the activation receipt did not own the candidate".to_owned(),
                win32_error: None,
            }),
        };
        std::fs::write(&health_path, serde_json::to_vec(&failure).unwrap()).unwrap();
        let status = SERVICE_STATUS_PROCESS {
            dwCurrentState: SERVICE_STOPPED,
            dwWin32ExitCode: 1066,
            dwServiceSpecificExitCode: 1,
            ..SERVICE_STATUS_PROCESS::default()
        };

        let error =
            stopped_before_expected_state_error(&status, SERVICE_RUNNING, Some(&health_path));
        let message = error.to_string();

        assert!(message.contains("win32=1066, service=1"));
        assert!(message.contains("activation-recovery-required"));
        assert!(message.contains("activation receipt did not own the candidate"));
    }
}
