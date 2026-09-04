use mactype_service_platform::JobObject;
#[cfg(test)]
use mactype_service_platform::Process;
use windows_sys::Win32::Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE};

pub(in crate::machine_integration::open_service) struct OwnedKernelHandle(
    pub(in crate::machine_integration::open_service) HANDLE,
);

impl OwnedKernelHandle {
    pub(in crate::machine_integration::open_service) fn raw(&self) -> HANDLE {
        self.0
    }
}

impl Drop for OwnedKernelHandle {
    fn drop(&mut self) {
        if !self.0.is_null() && self.0 != INVALID_HANDLE_VALUE {
            unsafe { CloseHandle(self.0) };
        }
    }
}

pub(in crate::machine_integration::open_service) struct KillOnCloseJob(JobObject);

impl KillOnCloseJob {
    pub(in crate::machine_integration::open_service) fn new() -> Result<Self, String> {
        JobObject::kill_on_close()
            .map(Self)
            .map_err(|error| error.to_string())
    }

    #[cfg(test)]
    pub(in crate::machine_integration::open_service) fn assign(
        &self,
        process: &Process,
    ) -> Result<(), String> {
        self.0.assign(process).map_err(|error| error.to_string())
    }

    pub(in crate::machine_integration::open_service) fn arm_current_process(
        self,
    ) -> Result<(), String> {
        // The elevated broker is short-lived. Keeping this handle until process exit makes
        // forced broker termination close the job and kill every inherited descendant.
        self.0
            .assign_current_process()
            .map_err(|error| error.to_string())
    }

    #[cfg(test)]
    pub(in crate::machine_integration::open_service) fn kill_on_close_enabled(
        &self,
    ) -> Result<bool, String> {
        self.0
            .limits()
            .map(|limits| limits.kill_on_close)
            .map_err(|error| error.to_string())
    }
}
