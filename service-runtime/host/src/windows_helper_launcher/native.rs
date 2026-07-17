use std::io;
use std::mem::size_of;
use std::ptr::{null, null_mut};
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::{
    CloseHandle, SetHandleInformation, ERROR_BROKEN_PIPE, HANDLE, HANDLE_FLAG_INHERIT,
    INVALID_HANDLE_VALUE, WAIT_FAILED, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows_sys::Win32::Security::SECURITY_ATTRIBUTES;
use windows_sys::Win32::Storage::FileSystem::ReadFile;
#[cfg(test)]
use windows_sys::Win32::System::JobObjects::QueryInformationJobObject;
use windows_sys::Win32::System::JobObjects::{
    CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
    TerminateJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_ACTIVE_PROCESS,
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
};
use windows_sys::Win32::System::Pipes::CreatePipe;
use windows_sys::Win32::System::Threading::{
    DeleteProcThreadAttributeList, GetProcessTimes, InitializeProcThreadAttributeList,
    TerminateProcess, UpdateProcThreadAttribute, WaitForSingleObject,
    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
};

const MAX_HELPER_OUTPUT_BYTES: usize = 1024;

pub(super) struct OwnedHandle(HANDLE);

impl OwnedHandle {
    pub(super) fn new(handle: HANDLE) -> io::Result<Self> {
        if handle.is_null() || handle == INVALID_HANDLE_VALUE {
            Err(io::Error::last_os_error())
        } else {
            Ok(Self(handle))
        }
    }

    pub(super) fn get(&self) -> HANDLE {
        self.0
    }
}

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        if !self.0.is_null() && self.0 != INVALID_HANDLE_VALUE {
            unsafe { CloseHandle(self.0) };
            self.0 = null_mut();
        }
    }
}

pub(super) struct JobObject(OwnedHandle);

impl JobObject {
    pub(super) fn new() -> io::Result<Self> {
        let handle = OwnedHandle::new(unsafe { CreateJobObjectW(null(), null()) })?;
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if unsafe {
            SetInformationJobObject(
                handle.get(),
                JobObjectExtendedLimitInformation,
                (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(Self(handle))
    }

    pub(super) fn handle(&self) -> HANDLE {
        self.0.get()
    }

    pub(super) fn terminate(&self, exit_code: u32) -> io::Result<()> {
        if unsafe { TerminateJobObject(self.handle(), exit_code) } == 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    #[cfg(test)]
    pub(super) fn query_limits(&self) -> io::Result<JOBOBJECT_EXTENDED_LIMIT_INFORMATION> {
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        if unsafe {
            QueryInformationJobObject(
                self.handle(),
                JobObjectExtendedLimitInformation,
                (&mut limits as *mut JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                null_mut(),
            )
        } == 0
        {
            Err(io::Error::last_os_error())
        } else {
            Ok(limits)
        }
    }
}

pub(super) struct AttributeList {
    storage: Vec<usize>,
    initialized: bool,
}

impl AttributeList {
    pub(super) fn with_handles(handles: &[HANDLE]) -> io::Result<Self> {
        let mut bytes = 0usize;
        unsafe { InitializeProcThreadAttributeList(null_mut(), 1, 0, &mut bytes) };
        if bytes == 0 {
            return Err(io::Error::last_os_error());
        }
        let words = bytes.div_ceil(size_of::<usize>());
        let mut list = Self {
            storage: vec![0usize; words],
            initialized: false,
        };
        if unsafe { InitializeProcThreadAttributeList(list.as_mut_ptr(), 1, 0, &mut bytes) } == 0 {
            return Err(io::Error::last_os_error());
        }
        list.initialized = true;
        if unsafe {
            UpdateProcThreadAttribute(
                list.as_mut_ptr(),
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST as usize,
                handles.as_ptr().cast(),
                std::mem::size_of_val(handles),
                null_mut(),
                null(),
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(list)
    }

    pub(super) fn as_mut_ptr(
        &mut self,
    ) -> windows_sys::Win32::System::Threading::LPPROC_THREAD_ATTRIBUTE_LIST {
        self.storage.as_mut_ptr().cast()
    }
}

impl Drop for AttributeList {
    fn drop(&mut self) {
        if self.initialized {
            unsafe { DeleteProcThreadAttributeList(self.as_mut_ptr()) };
        }
    }
}

pub(super) fn inherited_pipe(
    security: &SECURITY_ATTRIBUTES,
) -> io::Result<(OwnedHandle, OwnedHandle)> {
    let mut read = null_mut();
    let mut write = null_mut();
    if unsafe { CreatePipe(&mut read, &mut write, security, 0) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let read = OwnedHandle::new(read)?;
    let write = OwnedHandle::new(write)?;
    if unsafe { SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((read, write))
}

pub(super) fn process_creation_time(process: HANDLE) -> io::Result<u64> {
    let mut creation = windows_sys::Win32::Foundation::FILETIME::default();
    let mut exit = windows_sys::Win32::Foundation::FILETIME::default();
    let mut kernel = windows_sys::Win32::Foundation::FILETIME::default();
    let mut user = windows_sys::Win32::Foundation::FILETIME::default();
    if unsafe { GetProcessTimes(process, &mut creation, &mut exit, &mut kernel, &mut user) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((u64::from(creation.dwHighDateTime) << 32) | u64::from(creation.dwLowDateTime))
}

pub(super) fn terminate_unassigned_child(child: &OwnedHandle, deadline: Instant) -> io::Result<()> {
    if unsafe { TerminateProcess(child.get(), 1) } == 0 {
        return Err(io::Error::last_os_error());
    }
    confirm_terminated(child, deadline)
}

pub(super) fn confirm_terminated(child: &OwnedHandle, deadline: Instant) -> io::Result<()> {
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "helper process termination could not be confirmed within the absolute bound",
            ));
        }
        let slice = remaining.min(Duration::from_millis(10)).as_millis() as u32;
        match unsafe { WaitForSingleObject(child.get(), slice.max(1)) } {
            WAIT_OBJECT_0 => return Ok(()),
            WAIT_TIMEOUT => continue,
            WAIT_FAILED => return Err(io::Error::last_os_error()),
            _ => return Err(io::Error::other("unexpected helper cleanup wait result")),
        }
    }
}

pub(super) fn read_bounded_output(read: OwnedHandle) -> io::Result<Vec<u8>> {
    let mut output = Vec::new();
    let mut buffer = [0u8; 512];
    loop {
        let mut bytes_read = 0;
        if unsafe {
            ReadFile(
                read.get(),
                buffer.as_mut_ptr().cast(),
                buffer.len() as u32,
                &mut bytes_read,
                null_mut(),
            )
        } == 0
        {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(ERROR_BROKEN_PIPE as i32) {
                break;
            }
            return Err(error);
        }
        if bytes_read == 0 {
            break;
        }
        let remaining = (MAX_HELPER_OUTPUT_BYTES + 1).saturating_sub(output.len());
        output.extend_from_slice(&buffer[..(bytes_read as usize).min(remaining)]);
    }
    Ok(output)
}
