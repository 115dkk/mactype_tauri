use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE},
    System::{
        JobObjects::{
            AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation,
            SetInformationJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
        },
        Threading::GetCurrentProcess,
    },
};

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

pub(in crate::machine_integration::open_service) struct KillOnCloseJob(OwnedKernelHandle);

impl KillOnCloseJob {
    pub(in crate::machine_integration::open_service) fn new() -> Result<Self, String> {
        let handle = unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) };
        if handle.is_null() {
            return Err(std::io::Error::last_os_error().to_string());
        }
        let handle = OwnedKernelHandle(handle);
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if unsafe {
            SetInformationJobObject(
                handle.raw(),
                JobObjectExtendedLimitInformation,
                (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )
        } == 0
        {
            return Err(std::io::Error::last_os_error().to_string());
        }
        Ok(Self(handle))
    }

    pub(in crate::machine_integration::open_service) fn assign(
        &self,
        process: HANDLE,
    ) -> Result<(), String> {
        if unsafe { AssignProcessToJobObject(self.0.raw(), process) } == 0 {
            Err(std::io::Error::last_os_error().to_string())
        } else {
            Ok(())
        }
    }

    pub(in crate::machine_integration::open_service) fn arm_current_process(
        self,
    ) -> Result<(), String> {
        self.assign(unsafe { GetCurrentProcess() })?;
        // The elevated broker is short-lived. Keeping this handle until process exit makes
        // forced broker termination close the job and kill every inherited descendant.
        std::mem::forget(self);
        Ok(())
    }

    #[cfg(test)]
    pub(in crate::machine_integration::open_service) fn kill_on_close_enabled(
        &self,
    ) -> Result<bool, String> {
        use windows_sys::Win32::System::JobObjects::QueryInformationJobObject;

        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        if unsafe {
            QueryInformationJobObject(
                self.0.raw(),
                JobObjectExtendedLimitInformation,
                (&mut limits as *mut JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                std::ptr::null_mut(),
            )
        } == 0
        {
            Err(std::io::Error::last_os_error().to_string())
        } else {
            Ok(limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE != 0)
        }
    }
}
