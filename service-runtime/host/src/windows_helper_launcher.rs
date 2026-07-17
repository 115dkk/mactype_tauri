mod command_line;
mod native;

#[cfg(test)]
mod tests;

use std::ffi::{OsStr, OsString};
use std::io;
use std::mem::size_of;
use std::os::windows::ffi::OsStrExt;
use std::ptr::{null, null_mut};
#[cfg(test)]
use std::sync::atomic::AtomicU32;
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::{ERROR_CANCELLED, WAIT_FAILED, WAIT_OBJECT_0, WAIT_TIMEOUT};
use windows_sys::Win32::Security::SECURITY_ATTRIBUTES;
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, FILE_ATTRIBUTE_NORMAL, FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_SHARE_READ,
    FILE_SHARE_WRITE, OPEN_EXISTING, SYNCHRONIZE,
};
use windows_sys::Win32::System::JobObjects::AssignProcessToJobObject;
use windows_sys::Win32::System::Threading::{
    CreateProcessW, GetExitCodeProcess, OpenProcess, ResumeThread, WaitForSingleObject,
    CREATE_NO_WINDOW, CREATE_SUSPENDED, EXTENDED_STARTUPINFO_PRESENT, PROCESS_CREATE_THREAD,
    PROCESS_INFORMATION, PROCESS_QUERY_INFORMATION, PROCESS_QUERY_LIMITED_INFORMATION,
    PROCESS_VM_OPERATION, PROCESS_VM_READ, PROCESS_VM_WRITE, STARTF_USESTDHANDLES, STARTUPINFOEXW,
};

use crate::{HelperInvocation, HelperLaunchError, HelperLauncher, HelperOutput};
use command_line::command_line;
use native::{
    confirm_terminated, inherited_pipe, process_creation_time, read_bounded_output,
    terminate_unassigned_child, AttributeList, JobObject, OwnedHandle,
};

const TERMINATION_CONFIRMATION: Duration = Duration::from_millis(250);
const TARGET_PROCESS_ACCESS: u32 = PROCESS_CREATE_THREAD
    | PROCESS_QUERY_INFORMATION
    | PROCESS_QUERY_LIMITED_INFORMATION
    | PROCESS_VM_OPERATION
    | PROCESS_VM_WRITE
    | PROCESS_VM_READ
    | SYNCHRONIZE;

#[cfg(test)]
static LAST_TEST_CHILD_PID: AtomicU32 = AtomicU32::new(0);

pub struct WindowsHelperLauncher {
    stop_requested: fn() -> bool,
}

impl WindowsHelperLauncher {
    pub const fn new(stop_requested: fn() -> bool) -> Self {
        Self { stop_requested }
    }

    fn launch_process<F>(
        &self,
        invocation: &HelperInvocation,
        arguments_for_handle: F,
    ) -> Result<HelperOutput, HelperLaunchError>
    where
        F: FnOnce(usize) -> Vec<OsString>,
    {
        if (self.stop_requested)() {
            return Err(io::Error::new(
                io::ErrorKind::Interrupted,
                "service stop was requested before helper launch",
            )
            .into());
        }
        let deadline = Instant::now() + invocation.timeout;
        let job = JobObject::new()?;
        let target = OwnedHandle::new(unsafe {
            OpenProcess(TARGET_PROCESS_ACCESS, 1, invocation.target.pid)
        })?;
        if process_creation_time(target.get())? != invocation.target.creation_time {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "target process creation time changed before helper launch",
            )
            .into());
        }

        let security = SECURITY_ATTRIBUTES {
            nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: null_mut(),
            bInheritHandle: 1,
        };
        let (output_read, output_write) = inherited_pipe(&security)?;
        let null_name: Vec<u16> = OsStr::new("NUL").encode_wide().chain(Some(0)).collect();
        let null_device = OwnedHandle::new(unsafe {
            CreateFileW(
                null_name.as_ptr(),
                FILE_GENERIC_READ | FILE_GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &security,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                null_mut(),
            )
        })?;

        let inherited = [target.get(), output_write.get(), null_device.get()];
        let mut attributes = AttributeList::with_handles(&inherited)?;
        let arguments = arguments_for_handle(target.get() as usize);
        let mut command_line = command_line(&invocation.executable, &arguments);
        let application: Vec<u16> = invocation
            .executable
            .as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect();
        let mut startup = STARTUPINFOEXW::default();
        startup.StartupInfo.cb = size_of::<STARTUPINFOEXW>() as u32;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = null_device.get();
        startup.StartupInfo.hStdOutput = output_write.get();
        startup.StartupInfo.hStdError = null_device.get();
        startup.lpAttributeList = attributes.as_mut_ptr();
        if (self.stop_requested)() {
            return Err(io::Error::new(
                io::ErrorKind::Interrupted,
                "service stop was requested before helper process creation",
            )
            .into());
        }

        let mut process = PROCESS_INFORMATION::default();
        if unsafe {
            CreateProcessW(
                application.as_ptr(),
                command_line.as_mut_ptr(),
                null(),
                null(),
                1,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                null(),
                null(),
                &startup.StartupInfo,
                &mut process,
            )
        } == 0
        {
            return Err(io::Error::last_os_error().into());
        }
        let child_process = OwnedHandle::new(process.hProcess)?;
        let child_thread = OwnedHandle::new(process.hThread)?;
        #[cfg(test)]
        LAST_TEST_CHILD_PID.store(process.dwProcessId, std::sync::atomic::Ordering::Release);
        if unsafe { AssignProcessToJobObject(job.handle(), child_process.get()) } == 0 {
            let error = io::Error::last_os_error();
            terminate_unassigned_child(&child_process, deadline)?;
            return Err(error.into());
        }
        if (self.stop_requested)() {
            job.terminate(ERROR_CANCELLED)?;
            confirm_terminated(&child_process, deadline)?;
            return Err(io::Error::new(
                io::ErrorKind::Interrupted,
                "service stop was requested before helper resume",
            )
            .into());
        }
        if unsafe { ResumeThread(child_thread.get()) } == u32::MAX {
            let error = io::Error::last_os_error();
            job.terminate(error.raw_os_error().unwrap_or(1) as u32)?;
            confirm_terminated(&child_process, deadline)?;
            return Err(error.into());
        }
        drop(output_write);
        drop(null_device);
        drop(target);
        drop(attributes);
        drop(child_thread);

        wait_for_helper(
            self.stop_requested,
            &job,
            &child_process,
            output_read,
            deadline,
            invocation.timeout,
        )
    }
}

impl HelperLauncher for WindowsHelperLauncher {
    fn launch(&self, invocation: &HelperInvocation) -> Result<HelperOutput, HelperLaunchError> {
        self.launch_process(invocation, |handle| {
            invocation.arguments_for_process_handle(handle)
        })
    }
}

fn wait_for_helper(
    stop_requested: fn() -> bool,
    job: &JobObject,
    child_process: &OwnedHandle,
    output_read: OwnedHandle,
    deadline: Instant,
    timeout: Duration,
) -> Result<HelperOutput, HelperLaunchError> {
    let execution_deadline = deadline
        .checked_sub(timeout.min(TERMINATION_CONFIRMATION))
        .unwrap_or(deadline);
    loop {
        if stop_requested() {
            let cleanup_deadline = deadline.min(Instant::now() + TERMINATION_CONFIRMATION);
            job.terminate(ERROR_CANCELLED)
                .map_err(HelperLaunchError::after_resume)?;
            confirm_terminated(child_process, cleanup_deadline)
                .map_err(HelperLaunchError::after_resume)?;
            return Err(HelperLaunchError::after_resume(io::Error::new(
                io::ErrorKind::Interrupted,
                "service stop terminated an in-flight helper with unknown target cleanup",
            )));
        }
        let remaining = execution_deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            job.terminate(1460)
                .map_err(HelperLaunchError::after_resume)?;
            confirm_terminated(child_process, deadline).map_err(HelperLaunchError::after_resume)?;
            return Err(HelperLaunchError::after_resume(io::Error::new(
                io::ErrorKind::TimedOut,
                "helper exceeded its absolute launch timeout",
            )));
        }
        let slice = remaining.min(Duration::from_millis(10)).as_millis() as u32;
        match unsafe { WaitForSingleObject(child_process.get(), slice.max(1)) } {
            WAIT_OBJECT_0 => break,
            WAIT_TIMEOUT => continue,
            WAIT_FAILED => return Err(HelperLaunchError::after_resume(io::Error::last_os_error())),
            _ => {
                return Err(HelperLaunchError::after_resume(io::Error::other(
                    "unexpected helper wait result",
                )))
            }
        }
    }
    let mut exit_code = 3;
    if unsafe { GetExitCodeProcess(child_process.get(), &mut exit_code) } == 0 {
        return Err(HelperLaunchError::after_resume(io::Error::last_os_error()));
    }
    let stdout = read_bounded_output(output_read).map_err(HelperLaunchError::after_resume)?;
    Ok(HelperOutput {
        exit_code: exit_code as i32,
        stdout,
    })
}
