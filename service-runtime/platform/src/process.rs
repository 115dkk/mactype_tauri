//! Process handles and the identity queries the service makes on them.

use std::io;
use std::mem::size_of;
use std::os::windows::ffi::OsStringExt;
use std::path::PathBuf;
use std::time::Duration;

use windows_sys::Win32::Foundation::{FILETIME, STILL_ACTIVE};
use windows_sys::Win32::Storage::FileSystem::SYNCHRONIZE;
use windows_sys::Win32::System::RemoteDesktop::ProcessIdToSessionId;
use windows_sys::Win32::System::SystemInformation::{
    IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_UNKNOWN,
};
use windows_sys::Win32::System::Threading::{
    GetCurrentProcess, GetExitCodeProcess, GetProcessInformation, GetProcessTimes,
    IsProcessCritical, IsWow64Process2, OpenProcess, ProcessProtectionLevelInfo,
    QueryFullProcessImageNameW, TerminateProcess, PROCESS_CREATE_THREAD, PROCESS_NAME_WIN32,
    PROCESS_PROTECTION_LEVEL_INFORMATION, PROCESS_QUERY_INFORMATION,
    PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_VM_OPERATION, PROCESS_VM_READ, PROCESS_VM_WRITE,
    PROTECTION_LEVEL_NONE,
};

use crate::handle::{OwnedHandle, WaitOutcome};

const MAX_IMAGE_PATH_UNITS: usize = 32_768;

/// The exact access a process handle is opened with. Each variant is a fixed
/// rights set the service needs; there is no way to request arbitrary rights.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProcessAccess {
    /// `PROCESS_QUERY_LIMITED_INFORMATION`: identity and liveness queries.
    QueryLimited,
    /// `SYNCHRONIZE` only: waiting for exit.
    Synchronize,
    /// The rights the fixed injection helper needs on its target. The handle
    /// is opened inheritable because it reaches the helper through handle
    /// inheritance and never by PID.
    InjectionTarget,
}

impl ProcessAccess {
    fn rights(self) -> u32 {
        match self {
            Self::QueryLimited => PROCESS_QUERY_LIMITED_INFORMATION,
            Self::Synchronize => SYNCHRONIZE,
            Self::InjectionTarget => {
                PROCESS_CREATE_THREAD
                    | PROCESS_QUERY_INFORMATION
                    | PROCESS_QUERY_LIMITED_INFORMATION
                    | PROCESS_VM_OPERATION
                    | PROCESS_VM_WRITE
                    | PROCESS_VM_READ
                    | SYNCHRONIZE
            }
        }
    }

    fn inheritable(self) -> bool {
        matches!(self, Self::InjectionTarget)
    }
}

/// The machine a process runs as, next to the machine the OS runs on.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProcessMachine {
    pub process: MachineKind,
    pub native: MachineKind,
}

/// An image machine, reduced to the cases the service can act on.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MachineKind {
    I386,
    Amd64,
    /// `IMAGE_FILE_MACHINE_UNKNOWN` in the process slot means "same as native".
    Unknown,
    Other(u16),
}

impl MachineKind {
    pub(crate) fn from_raw(value: u16) -> Self {
        match value {
            IMAGE_FILE_MACHINE_I386 => Self::I386,
            IMAGE_FILE_MACHINE_AMD64 => Self::Amd64,
            IMAGE_FILE_MACHINE_UNKNOWN => Self::Unknown,
            other => Self::Other(other),
        }
    }
}

/// An open process handle with a fixed rights set.
#[derive(Debug)]
pub struct Process {
    handle: OwnedHandle,
    access: ProcessAccess,
}

impl Process {
    /// Opens `pid` with exactly `access`. The error carries the Win32 code; a
    /// PID that has left the process table reports `ERROR_INVALID_PARAMETER`.
    pub fn open(pid: u32, access: ProcessAccess) -> io::Result<Self> {
        // SAFETY: OpenProcess takes plain integers and returns a new handle or
        // null; no memory is shared with the callee.
        let handle = unsafe { OpenProcess(access.rights(), i32::from(access.inheritable()), pid) };
        Ok(Self {
            handle: OwnedHandle::from_creation(handle)?,
            access,
        })
    }

    pub(crate) fn from_owned(handle: OwnedHandle, access: ProcessAccess) -> Self {
        Self { handle, access }
    }

    pub fn access(&self) -> ProcessAccess {
        self.access
    }

    /// The owned handle, for handle-inheritance lists and waits.
    pub fn handle(&self) -> &OwnedHandle {
        &self.handle
    }

    /// Creation time as a 64-bit `FILETIME`, the value the service uses with
    /// the PID to identify a process across its whole lifetime.
    pub fn creation_time(&self) -> io::Result<u64> {
        let mut creation = FILETIME::default();
        let mut exit = FILETIME::default();
        let mut kernel = FILETIME::default();
        let mut user = FILETIME::default();
        // SAFETY: the handle is live and every out pointer refers to a local
        // `FILETIME` that outlives the call.
        if unsafe {
            GetProcessTimes(
                self.handle.as_raw(),
                &mut creation,
                &mut exit,
                &mut kernel,
                &mut user,
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok((u64::from(creation.dwHighDateTime) << 32) | u64::from(creation.dwLowDateTime))
    }

    /// The process and native machine reported by `IsWow64Process2`.
    pub fn machine(&self) -> io::Result<ProcessMachine> {
        let mut process = IMAGE_FILE_MACHINE_UNKNOWN;
        let mut native = IMAGE_FILE_MACHINE_UNKNOWN;
        // SAFETY: the handle is live; both out pointers are local `u16`s.
        if unsafe { IsWow64Process2(self.handle.as_raw(), &mut process, &mut native) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(ProcessMachine {
            process: MachineKind::from_raw(process),
            native: MachineKind::from_raw(native),
        })
    }

    /// `Some(code)` once the process has exited, `None` while it still runs.
    pub fn exit_code(&self) -> io::Result<Option<u32>> {
        let mut exit_code = 0_u32;
        // SAFETY: the handle is live; the out pointer is a local `u32`.
        if unsafe { GetExitCodeProcess(self.handle.as_raw(), &mut exit_code) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok((exit_code != STILL_ACTIVE as u32).then_some(exit_code))
    }

    /// Whether the process runs at any protection level, or whether that
    /// cannot be determined. Both answers keep the service away from it.
    pub fn is_protected_or_unknown(&self) -> bool {
        let mut information = PROCESS_PROTECTION_LEVEL_INFORMATION::default();
        // SAFETY: the handle is live; the buffer pointer and length describe
        // exactly the local `PROCESS_PROTECTION_LEVEL_INFORMATION`.
        if unsafe {
            GetProcessInformation(
                self.handle.as_raw(),
                ProcessProtectionLevelInfo,
                (&mut information as *mut PROCESS_PROTECTION_LEVEL_INFORMATION).cast(),
                size_of::<PROCESS_PROTECTION_LEVEL_INFORMATION>() as u32,
            )
        } == 0
        {
            return true;
        }
        information.ProtectionLevel != PROTECTION_LEVEL_NONE
    }

    /// Whether the process is marked critical, or whether that cannot be
    /// determined. Both answers keep the service away from it.
    pub fn is_critical_or_unknown(&self) -> bool {
        let mut critical = 0;
        // SAFETY: the handle is live; the out pointer is a local `BOOL`.
        let queried = unsafe { IsProcessCritical(self.handle.as_raw(), &mut critical) };
        queried == 0 || critical != 0
    }

    /// The Win32 image path, or `None` when it cannot be read.
    pub fn image_path(&self) -> Option<PathBuf> {
        let mut buffer = vec![0_u16; MAX_IMAGE_PATH_UNITS];
        let mut length = buffer.len() as u32;
        // SAFETY: the handle is live; `buffer` is writable for `length` units
        // and the API writes at most that many, updating `length` to the
        // number it filled.
        if unsafe {
            QueryFullProcessImageNameW(
                self.handle.as_raw(),
                PROCESS_NAME_WIN32,
                buffer.as_mut_ptr(),
                &mut length,
            )
        } == 0
        {
            return None;
        }
        let length = (length as usize).min(buffer.len());
        Some(PathBuf::from(std::ffi::OsString::from_wide(
            &buffer[..length],
        )))
    }

    /// Terminates the process with `exit_code`.
    pub fn terminate(&self, exit_code: u32) -> io::Result<()> {
        // SAFETY: the handle is live; the call takes no pointers.
        if unsafe { TerminateProcess(self.handle.as_raw(), exit_code) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    /// Waits for the process to exit.
    pub fn wait(&self, timeout: Option<Duration>) -> io::Result<WaitOutcome> {
        self.handle.wait(timeout)
    }
}

/// The session a PID belongs to.
pub fn process_session_id(pid: u32) -> io::Result<u32> {
    let mut session_id = 0;
    // SAFETY: plain integer in, local `u32` out.
    if unsafe { ProcessIdToSessionId(pid, &mut session_id) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(session_id)
}

/// Ends the current process immediately with `exit_code`, without running
/// destructors. Used only by the CI crash adapter to simulate a service crash.
pub fn terminate_current_process(exit_code: u32) -> io::Error {
    // SAFETY: the pseudo-handle from GetCurrentProcess is always valid and is
    // never closed; on success the call does not return.
    unsafe { TerminateProcess(GetCurrentProcess(), exit_code) };
    io::Error::last_os_error()
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use windows_sys::Win32::Foundation::ERROR_INVALID_PARAMETER;

    use super::{process_session_id, MachineKind, Process, ProcessAccess};
    use crate::handle::WaitOutcome;

    #[test]
    fn the_current_process_answers_every_identity_query() {
        let process = Process::open(std::process::id(), ProcessAccess::QueryLimited).unwrap();
        assert!(process.creation_time().unwrap() > 0);
        assert_eq!(process.exit_code().unwrap(), None);
        assert!(!process.is_critical_or_unknown());
        assert!(!process.is_protected_or_unknown());
        let machine = process.machine().unwrap();
        assert!(matches!(
            machine.native,
            MachineKind::Amd64 | MachineKind::I386 | MachineKind::Other(_)
        ));
        let image = process.image_path().unwrap();
        assert!(image.file_name().is_some());
        assert_eq!(
            process_session_id(std::process::id()).unwrap(),
            process_session_id(std::process::id()).unwrap()
        );
    }

    #[test]
    fn an_exited_child_reports_its_exit_code_and_a_dead_pid_is_invalid() {
        let mut child = std::process::Command::new("cmd")
            .args(["/d", "/c", "exit 7"])
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .unwrap();
        let process = Process::open(child.id(), ProcessAccess::Synchronize).unwrap();
        child.wait().unwrap();
        assert_eq!(
            process.wait(Some(Duration::from_secs(5))).unwrap(),
            WaitOutcome::Signaled
        );
        if let Ok(queryable) = Process::open(child.id(), ProcessAccess::QueryLimited) {
            assert_eq!(queryable.exit_code().unwrap(), Some(7));
        }
        drop(process);

        let error = Process::open(u32::MAX - 1, ProcessAccess::QueryLimited).unwrap_err();
        assert_eq!(error.raw_os_error(), Some(ERROR_INVALID_PARAMETER as i32));
    }
}
