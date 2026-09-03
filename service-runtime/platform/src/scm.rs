//! Service Control Manager handles and the operations the setup broker and
//! the startup safety probe perform through them.

use std::ffi::c_void;
use std::io;
use std::mem::size_of;
use std::ptr::{null, null_mut};

use windows_sys::Win32::Foundation::{
    ERROR_SERVICE_ALREADY_RUNNING, ERROR_SERVICE_DOES_NOT_EXIST, ERROR_SERVICE_NOT_ACTIVE,
};
use windows_sys::Win32::System::Services::{
    ChangeServiceConfig2W, ChangeServiceConfigW, CloseServiceHandle, ControlService,
    CreateServiceW, DeleteService, OpenSCManagerW, OpenServiceW, QueryServiceConfigW,
    QueryServiceStatusEx, StartServiceW, QUERY_SERVICE_CONFIGW, SC_ACTION, SC_ACTION_RESTART,
    SC_HANDLE, SC_MANAGER_CONNECT, SC_MANAGER_CREATE_SERVICE, SC_STATUS_PROCESS_INFO,
    SERVICE_ALL_ACCESS, SERVICE_AUTO_START, SERVICE_CHANGE_CONFIG, SERVICE_CONFIG_DESCRIPTION,
    SERVICE_CONFIG_FAILURE_ACTIONS, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, SERVICE_CONTINUE_PENDING,
    SERVICE_CONTROL_STOP, SERVICE_DESCRIPTIONW, SERVICE_ERROR_NORMAL, SERVICE_FAILURE_ACTIONSW,
    SERVICE_FAILURE_ACTIONS_FLAG, SERVICE_NO_CHANGE, SERVICE_PAUSED, SERVICE_PAUSE_PENDING,
    SERVICE_QUERY_CONFIG, SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_START,
    SERVICE_START_PENDING, SERVICE_STATUS, SERVICE_STATUS_PROCESS, SERVICE_STOP, SERVICE_STOPPED,
    SERVICE_STOP_PENDING, SERVICE_WIN32_OWN_PROCESS,
};

use crate::scm_response::ScmResponse;
use crate::wide::wide_null;

const MAX_SERVICE_DEPENDENCIES: usize = 256;
const SERVICE_DELETE: u32 = 0x0001_0000;

/// What the manager connection may do.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ServiceManagerAccess {
    Connect,
    ConnectAndCreate,
}

/// What an opened service handle may do. Each variant is a fixed rights set.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ServiceAccess {
    QueryStatus,
    QueryConfig,
    QueryStatusAndConfig,
    Start,
    Stop,
    Reconfigure,
    Delete,
}

impl ServiceAccess {
    fn rights(self) -> u32 {
        match self {
            Self::QueryStatus => SERVICE_QUERY_STATUS,
            Self::QueryConfig => SERVICE_QUERY_CONFIG,
            Self::QueryStatusAndConfig => SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
            Self::Start => SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
            Self::Stop => SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
            Self::Reconfigure => {
                SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_START
            }
            Self::Delete => SERVICE_DELETE | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
        }
    }
}

/// A service state as the SCM reports it.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ServiceState {
    Stopped,
    StartPending,
    StopPending,
    Running,
    ContinuePending,
    PausePending,
    Paused,
    Other(u32),
}

impl ServiceState {
    pub fn from_raw(value: u32) -> Self {
        match value {
            SERVICE_STOPPED => Self::Stopped,
            SERVICE_START_PENDING => Self::StartPending,
            SERVICE_STOP_PENDING => Self::StopPending,
            SERVICE_RUNNING => Self::Running,
            SERVICE_CONTINUE_PENDING => Self::ContinuePending,
            SERVICE_PAUSE_PENDING => Self::PausePending,
            SERVICE_PAUSED => Self::Paused,
            other => Self::Other(other),
        }
    }

    pub fn as_raw(self) -> u32 {
        match self {
            Self::Stopped => SERVICE_STOPPED,
            Self::StartPending => SERVICE_START_PENDING,
            Self::StopPending => SERVICE_STOP_PENDING,
            Self::Running => SERVICE_RUNNING,
            Self::ContinuePending => SERVICE_CONTINUE_PENDING,
            Self::PausePending => SERVICE_PAUSE_PENDING,
            Self::Paused => SERVICE_PAUSED,
            Self::Other(value) => value,
        }
    }
}

/// The process-level status of a service.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ServiceStatusSnapshot {
    pub state: ServiceState,
    pub process_id: u32,
    pub win32_exit_code: u32,
    pub service_specific_exit_code: u32,
}

/// A service's configuration, copied into owned strings.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServiceConfig {
    pub service_type: u32,
    pub start_type: u32,
    pub error_control: u32,
    pub image_path: String,
    pub account: String,
    pub display_name: String,
    pub load_order_group: String,
    pub tag_id: u32,
    pub dependencies: Vec<String>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StartOutcome {
    Started,
    AlreadyRunning,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StopOutcome {
    StopRequested,
    NotActive,
}

/// A connection to the SCM.
#[derive(Debug)]
pub struct ServiceControlManager(ScHandle);

impl ServiceControlManager {
    pub fn connect(access: ServiceManagerAccess) -> io::Result<Self> {
        let rights = match access {
            ServiceManagerAccess::Connect => SC_MANAGER_CONNECT,
            ServiceManagerAccess::ConnectAndCreate => {
                SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE
            }
        };
        // SAFETY: null machine and database names select the local active
        // database; the call takes no other pointers.
        let handle = unsafe { OpenSCManagerW(null(), null(), rights) };
        Ok(Self(ScHandle::from_open(handle)?))
    }

    /// Opens `name`. `Ok(None)` when no such service exists; other failures,
    /// including a delete-pending record, carry their Win32 code.
    pub fn open(&self, name: &str, access: ServiceAccess) -> io::Result<Option<ServiceHandle>> {
        let name = wide_null(name);
        // SAFETY: the manager handle is live and `name` is NUL-terminated.
        let handle = unsafe { OpenServiceW(self.0.as_raw(), name.as_ptr(), access.rights()) };
        if handle.is_null() {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(ERROR_SERVICE_DOES_NOT_EXIST as i32) {
                return Ok(None);
            }
            return Err(error);
        }
        Ok(Some(ServiceHandle(ScHandle(handle))))
    }

    /// Creates an auto-start, own-process, LocalSystem service with full
    /// access on the returned handle.
    pub fn create_own_process_auto_start(
        &self,
        name: &str,
        display_name: &str,
        image_path: &str,
    ) -> io::Result<ServiceHandle> {
        let name = wide_null(name);
        let display_name = wide_null(display_name);
        let image_path = wide_null(image_path);
        // SAFETY: the manager handle is live and every string is
        // NUL-terminated; the null arguments select no load order group, tag,
        // dependencies, account (LocalSystem), or password.
        let handle = unsafe {
            CreateServiceW(
                self.0.as_raw(),
                name.as_ptr(),
                display_name.as_ptr(),
                SERVICE_ALL_ACCESS,
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL,
                image_path.as_ptr(),
                null(),
                null_mut(),
                null(),
                null(),
                null(),
            )
        };
        Ok(ServiceHandle(ScHandle::from_open(handle)?))
    }
}

/// An open service handle.
#[derive(Debug)]
pub struct ServiceHandle(ScHandle);

impl ServiceHandle {
    pub fn status(&self) -> io::Result<ServiceStatusSnapshot> {
        let mut status = SERVICE_STATUS_PROCESS::default();
        let mut needed = 0;
        // SAFETY: the handle is live; the buffer pointer and length describe
        // exactly the local status structure.
        if unsafe {
            QueryServiceStatusEx(
                self.0.as_raw(),
                SC_STATUS_PROCESS_INFO,
                (&mut status as *mut SERVICE_STATUS_PROCESS).cast::<u8>(),
                size_of::<SERVICE_STATUS_PROCESS>() as u32,
                &mut needed,
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(ServiceStatusSnapshot {
            state: ServiceState::from_raw(status.dwCurrentState),
            process_id: status.dwProcessId,
            win32_exit_code: status.dwWin32ExitCode,
            service_specific_exit_code: status.dwServiceSpecificExitCode,
        })
    }

    pub fn config(&self) -> io::Result<ServiceConfig> {
        let response = ScmResponse::query(
            size_of::<QUERY_SERVICE_CONFIGW>(),
            |buffer, capacity, needed| {
                // SAFETY: the handle is live; `buffer` and `capacity` describe
                // the response allocation or the documented null size probe,
                // and `needed` is a local out value.
                unsafe { QueryServiceConfigW(self.0.as_raw(), buffer.cast(), capacity, needed) }
            },
        )?;
        parse_config(&response)
    }

    pub fn start(&self) -> io::Result<StartOutcome> {
        // SAFETY: the handle is live; no arguments are passed to the service.
        if unsafe { StartServiceW(self.0.as_raw(), 0, null()) } == 0 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(ERROR_SERVICE_ALREADY_RUNNING as i32) {
                return Ok(StartOutcome::AlreadyRunning);
            }
            return Err(error);
        }
        Ok(StartOutcome::Started)
    }

    pub fn stop(&self) -> io::Result<StopOutcome> {
        let mut status = SERVICE_STATUS::default();
        // SAFETY: the handle is live; `status` is a local out structure.
        if unsafe { ControlService(self.0.as_raw(), SERVICE_CONTROL_STOP, &mut status) } == 0 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(ERROR_SERVICE_NOT_ACTIVE as i32) {
                return Ok(StopOutcome::NotActive);
            }
            return Err(error);
        }
        Ok(StopOutcome::StopRequested)
    }

    pub fn delete(&self) -> io::Result<()> {
        // SAFETY: the handle is live; the call takes no pointers.
        if unsafe { DeleteService(self.0.as_raw()) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    /// Rewrites the image path and display name and forces auto start,
    /// leaving every other field unchanged.
    pub fn set_image_and_display_name(
        &self,
        image_path: &str,
        display_name: &str,
    ) -> io::Result<()> {
        let image_path = wide_null(image_path);
        let display_name = wide_null(display_name);
        // SAFETY: the handle is live; both strings are NUL-terminated; the
        // remaining arguments are the documented "no change" values.
        if unsafe {
            ChangeServiceConfigW(
                self.0.as_raw(),
                SERVICE_NO_CHANGE,
                SERVICE_AUTO_START,
                SERVICE_NO_CHANGE,
                image_path.as_ptr(),
                null(),
                null_mut(),
                null(),
                null(),
                null(),
                display_name.as_ptr(),
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    pub fn set_description(&self, text: &str) -> io::Result<()> {
        let mut text = wide_null(text);
        let description = SERVICE_DESCRIPTIONW {
            lpDescription: text.as_mut_ptr(),
        };
        // SAFETY: the description structure and the buffer it points at
        // outlive the call.
        self.change_config2(
            SERVICE_CONFIG_DESCRIPTION,
            (&description as *const SERVICE_DESCRIPTIONW).cast(),
        )
    }

    /// Restart-on-failure actions with the given delays, counted from a reset
    /// period in seconds.
    pub fn set_restart_actions(
        &self,
        reset_period_seconds: u32,
        delays_ms: &[u32],
    ) -> io::Result<()> {
        let mut actions: Vec<SC_ACTION> = delays_ms
            .iter()
            .map(|delay| SC_ACTION {
                Type: SC_ACTION_RESTART,
                Delay: *delay,
            })
            .collect();
        let failure = SERVICE_FAILURE_ACTIONSW {
            dwResetPeriod: reset_period_seconds,
            lpRebootMsg: null_mut(),
            lpCommand: null_mut(),
            cActions: actions.len() as u32,
            lpsaActions: actions.as_mut_ptr(),
        };
        // SAFETY: the failure structure and the action array outlive the call.
        self.change_config2(
            SERVICE_CONFIG_FAILURE_ACTIONS,
            (&failure as *const SERVICE_FAILURE_ACTIONSW).cast(),
        )
    }

    pub fn set_failure_actions_on_non_crash_failures(&self, enabled: bool) -> io::Result<()> {
        let flag = SERVICE_FAILURE_ACTIONS_FLAG {
            fFailureActionsOnNonCrashFailures: i32::from(enabled),
        };
        // SAFETY: the flag structure outlives the call.
        self.change_config2(
            SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
            (&flag as *const SERVICE_FAILURE_ACTIONS_FLAG).cast(),
        )
    }

    fn change_config2(&self, level: u32, data: *const c_void) -> io::Result<()> {
        // SAFETY: the handle is live and `data` points at a live structure of
        // the type `level` selects, kept alive by the caller.
        if unsafe { ChangeServiceConfig2W(self.0.as_raw(), level, data) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }
}

fn parse_config(response: &ScmResponse) -> io::Result<ServiceConfig> {
    let config = response.header::<QUERY_SERVICE_CONFIGW>()?;
    let text = |pointer| -> io::Result<String> {
        Ok(response
            .wide_units(pointer)?
            .map(String::from_utf16_lossy)
            .unwrap_or_default())
    };
    Ok(ServiceConfig {
        service_type: config.dwServiceType,
        start_type: config.dwStartType,
        error_control: config.dwErrorControl,
        image_path: text(config.lpBinaryPathName)?,
        account: text(config.lpServiceStartName)?,
        display_name: text(config.lpDisplayName)?,
        load_order_group: text(config.lpLoadOrderGroup)?,
        tag_id: config.dwTagId,
        dependencies: response
            .multi_units(config.lpDependencies, MAX_SERVICE_DEPENDENCIES)?
            .into_iter()
            .map(String::from_utf16_lossy)
            .collect(),
    })
}

/// An `SC_HANDLE` closed exactly once.
#[derive(Debug)]
struct ScHandle(SC_HANDLE);

impl ScHandle {
    fn from_open(handle: SC_HANDLE) -> io::Result<Self> {
        if handle.is_null() {
            return Err(io::Error::last_os_error());
        }
        Ok(Self(handle))
    }

    fn as_raw(&self) -> SC_HANDLE {
        self.0
    }
}

impl Drop for ScHandle {
    fn drop(&mut self) {
        // SAFETY: the handle came from an SCM open or create call and is
        // closed once.
        unsafe { CloseServiceHandle(self.0) };
    }
}

#[cfg(test)]
mod tests {
    use std::mem::{size_of, size_of_val};
    use std::ptr::{null_mut, write};

    use windows_sys::Win32::System::Services::QUERY_SERVICE_CONFIGW;

    use super::{
        parse_config, ServiceAccess, ServiceControlManager, ServiceManagerAccess, ServiceState,
    };
    use crate::scm_response::ScmResponse;

    fn append_units(storage: &mut [u16], cursor: &mut usize, units: &[u16]) -> *mut u16 {
        let offset = *cursor;
        storage[offset..offset + units.len()].copy_from_slice(units);
        *cursor += units.len();
        storage.as_mut_ptr().wrapping_add(offset)
    }

    fn synthetic_config_response(dependencies: &[u16]) -> ScmResponse {
        let mut words = vec![0_usize; 64];
        let capacity = size_of_val(words.as_slice());
        // SAFETY: the slice covers the live word allocation as UTF-16 units;
        // `usize` alignment satisfies `u16` alignment.
        let storage = unsafe {
            std::slice::from_raw_parts_mut(
                words.as_mut_ptr().cast::<u16>(),
                capacity / size_of::<u16>(),
            )
        };
        let mut cursor = size_of::<QUERY_SERVICE_CONFIGW>().div_ceil(size_of::<u16>());
        let image_path = append_units(
            storage,
            &mut cursor,
            &"C:\\service.exe\0".encode_utf16().collect::<Vec<_>>(),
        );
        let account = append_units(
            storage,
            &mut cursor,
            &"LocalSystem\0".encode_utf16().collect::<Vec<_>>(),
        );
        let display_name = append_units(
            storage,
            &mut cursor,
            &"Synthetic Service\0".encode_utf16().collect::<Vec<_>>(),
        );
        let load_order_group = append_units(
            storage,
            &mut cursor,
            &"NetworkProvider\0".encode_utf16().collect::<Vec<_>>(),
        );
        let dependencies = append_units(storage, &mut cursor, dependencies);
        let header = QUERY_SERVICE_CONFIGW {
            dwServiceType: 0x20,
            dwStartType: 2,
            dwErrorControl: 1,
            lpBinaryPathName: image_path,
            lpLoadOrderGroup: load_order_group,
            dwTagId: 7,
            lpDependencies: dependencies,
            lpServiceStartName: account,
            lpDisplayName: display_name,
        };
        // SAFETY: the word buffer is aligned for QUERY_SERVICE_CONFIGW and has
        // enough space for the complete header at its start.
        unsafe { write(words.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>(), header) };
        ScmResponse::from_words(words, cursor * size_of::<u16>())
    }

    #[test]
    fn parse_config_reads_every_dependency_of_a_multi_string_block() {
        let dependencies: Vec<u16> = "RPCSS\0http\0\0".encode_utf16().collect();
        let response = synthetic_config_response(&dependencies);
        let config = parse_config(&response).unwrap();

        assert_eq!(config.dependencies, ["RPCSS", "http"]);
        assert_eq!(config.image_path, "C:\\service.exe");
        assert_eq!(config.account, "LocalSystem");
        assert_eq!(config.display_name, "Synthetic Service");
        assert_eq!(config.load_order_group, "NetworkProvider");
    }

    #[test]
    fn parse_config_rejects_a_dependency_block_without_its_terminator() {
        let dependencies: Vec<u16> = "RPCSS\0http\0".encode_utf16().collect();
        let response = synthetic_config_response(&dependencies);
        assert!(parse_config(&response).is_err());
    }

    #[test]
    fn parse_config_rejects_a_string_pointer_outside_the_response() {
        let mut words = vec![0_usize; 16];
        let outside = words.as_mut_ptr().cast::<u16>().wrapping_sub(1);
        let header = QUERY_SERVICE_CONFIGW {
            dwServiceType: 0,
            dwStartType: 0,
            dwErrorControl: 0,
            lpBinaryPathName: outside,
            lpLoadOrderGroup: null_mut(),
            dwTagId: 0,
            lpDependencies: null_mut(),
            lpServiceStartName: null_mut(),
            lpDisplayName: null_mut(),
        };
        // SAFETY: the word buffer is aligned for QUERY_SERVICE_CONFIGW and has
        // enough space for the complete header at its start.
        unsafe { write(words.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>(), header) };
        let byte_length = size_of_val(words.as_slice());
        let response = ScmResponse::from_words(words, byte_length);
        assert!(parse_config(&response).is_err());
    }

    #[test]
    fn a_well_known_service_reports_status_and_config_and_a_missing_one_is_none() {
        let manager = ServiceControlManager::connect(ServiceManagerAccess::Connect).unwrap();
        let service = manager
            .open("EventLog", ServiceAccess::QueryStatusAndConfig)
            .unwrap()
            .expect("the Windows Event Log service exists");
        let status = service.status().unwrap();
        assert!(matches!(
            status.state,
            ServiceState::Running | ServiceState::Stopped | ServiceState::StartPending
        ));
        let config = service.config().unwrap();
        assert!(config.image_path.to_ascii_lowercase().contains("svchost"));
        assert!(!config.display_name.is_empty());
        assert!(manager
            .open(
                "mactype-platform-missing-service",
                ServiceAccess::QueryStatus
            )
            .unwrap()
            .is_none());
    }

    #[test]
    fn lanman_workstation_reports_all_of_its_dependencies() {
        let manager = ServiceControlManager::connect(ServiceManagerAccess::Connect).unwrap();
        let service = manager
            .open("LanmanWorkstation", ServiceAccess::QueryConfig)
            .unwrap()
            .expect("the Workstation service exists");
        let config = service.config().unwrap();

        assert!(config.dependencies.len() >= 2);
        assert!(config
            .dependencies
            .iter()
            .any(|dependency| dependency.eq_ignore_ascii_case("NSI")));
    }
}
