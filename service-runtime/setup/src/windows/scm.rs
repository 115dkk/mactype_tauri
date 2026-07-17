mod configuration;
mod health;
mod lifecycle;
mod observation;

use std::io;
use std::path::PathBuf;
use std::ptr;
use std::time::Duration;

use mactype_service_contract::effective_service_name;
use windows_sys::Win32::Foundation::{GetLastError, ERROR_SERVICE_DOES_NOT_EXIST};
use windows_sys::Win32::System::Services::{
    CloseServiceHandle, OpenSCManagerW, OpenServiceW, SC_HANDLE, SC_MANAGER_CONNECT,
    SC_MANAGER_CREATE_SERVICE,
};

use configuration::{query_config, service_configuration_matches_owned_contract as owns_config};
pub use configuration::{
    service_configuration_matches_owned_contract, service_image_matches_protected_contract,
    ObservedServiceConfiguration,
};

use crate::SetupError;

const DISPLAY_NAME: &str = "MacType Control Center Service";
const DESCRIPTION: &str = "Runs the open MacType machine integration runtime.";
const STATE_TIMEOUT: Duration = Duration::from_secs(30);
const HEALTH_TIMEOUT: Duration = Duration::from_secs(20);

pub struct ServiceManager {
    handle: ServiceHandle,
    protected_root: PathBuf,
}

impl ServiceManager {
    pub fn connect(protected_root: PathBuf) -> Result<Self, SetupError> {
        let handle = unsafe {
            OpenSCManagerW(
                ptr::null(),
                ptr::null(),
                SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE,
            )
        };
        if handle.is_null() {
            return Err(SetupError::Io(io::Error::last_os_error()));
        }
        Ok(Self {
            handle: ServiceHandle(handle),
            protected_root,
        })
    }

    fn open_service(&self, access: u32) -> Result<Option<ServiceHandle>, io::Error> {
        self.open_named_service(effective_service_name(), access)
    }

    fn open_named_service(
        &self,
        service_name: &str,
        access: u32,
    ) -> Result<Option<ServiceHandle>, io::Error> {
        let service_name = wide(service_name);
        let handle = unsafe { OpenServiceW(self.handle.0, service_name.as_ptr(), access) };
        if handle.is_null() {
            let error = unsafe { GetLastError() };
            if error == ERROR_SERVICE_DOES_NOT_EXIST {
                return Ok(None);
            }
            return Err(io::Error::from_raw_os_error(error as i32));
        }
        Ok(Some(ServiceHandle(handle)))
    }

    fn ensure_owned(&self, service: &ServiceHandle) -> Result<(), SetupError> {
        let config = query_config(service.0)?;
        if !owns_config(&self.protected_root, &config.observed()) {
            return Err(SetupError::Runtime(
                "the fixed service name has a foreign configuration; refusing to mutate it"
                    .to_owned(),
            ));
        }
        Ok(())
    }
}

struct ServiceHandle(SC_HANDLE);

impl Drop for ServiceHandle {
    fn drop(&mut self) {
        unsafe {
            CloseServiceHandle(self.0);
        }
    }
}

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}
