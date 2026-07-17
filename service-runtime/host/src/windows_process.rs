use std::ffi::OsString;
use std::mem::size_of;
use std::os::windows::ffi::OsStringExt;

use mactype_service_contract::StructuredServiceError;
use windows_sys::Win32::Foundation::{CloseHandle, FILETIME, HANDLE};
use windows_sys::Win32::System::RemoteDesktop::ProcessIdToSessionId;
use windows_sys::Win32::System::SystemInformation::{
    IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_UNKNOWN,
};
use windows_sys::Win32::System::Threading::{
    GetProcessInformation, GetProcessTimes, IsProcessCritical, IsWow64Process2, OpenProcess,
    ProcessProtectionLevelInfo, QueryFullProcessImageNameW, PROCESS_NAME_WIN32,
    PROCESS_PROTECTION_LEVEL_INFORMATION, PROCESS_QUERY_LIMITED_INFORMATION, PROTECTION_LEVEL_NONE,
};

use crate::{ProcessArchitecture, ProcessIdentity, ProcessInspector};

pub struct WindowsProcessInspector {
    service_pid: u32,
}

impl WindowsProcessInspector {
    pub const fn new(service_pid: u32) -> Self {
        Self { service_pid }
    }
}

impl ProcessInspector for WindowsProcessInspector {
    fn inspect(&self, pid: u32) -> Result<ProcessIdentity, StructuredServiceError> {
        if pid == 0 {
            return Err(service_error(
                "process-identity-invalid",
                "process ID zero cannot be inspected",
                None,
            ));
        }
        let process = OwnedHandle::open(pid)?;
        let creation_time = process.creation_time()?;
        let session_id = process.session_id(pid)?;
        let architecture = process.architecture()?;
        let protected = process.is_protected();
        let excluded_from_injection = pid == self.service_pid || process.must_skip_injection();
        Ok(ProcessIdentity {
            pid,
            creation_time,
            session_id,
            architecture,
            protected,
            critical: excluded_from_injection,
        })
    }
}

struct OwnedHandle(HANDLE);

impl OwnedHandle {
    fn open(pid: u32) -> Result<Self, StructuredServiceError> {
        let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
        if handle.is_null() {
            return Err(service_error(
                "process-protected-or-inaccessible",
                "the observed process cannot be opened for identity verification",
                std::io::Error::last_os_error().raw_os_error(),
            ));
        }
        Ok(Self(handle))
    }

    fn creation_time(&self) -> Result<u64, StructuredServiceError> {
        let mut creation = FILETIME::default();
        let mut exit = FILETIME::default();
        let mut kernel = FILETIME::default();
        let mut user = FILETIME::default();
        if unsafe { GetProcessTimes(self.0, &mut creation, &mut exit, &mut kernel, &mut user) } == 0
        {
            return Err(last_error(
                "process-creation-time-unavailable",
                "the observed process creation time could not be read",
            ));
        }
        Ok((u64::from(creation.dwHighDateTime) << 32) | u64::from(creation.dwLowDateTime))
    }

    fn session_id(&self, pid: u32) -> Result<u32, StructuredServiceError> {
        let mut session_id = 0;
        if unsafe { ProcessIdToSessionId(pid, &mut session_id) } == 0 {
            return Err(last_error(
                "process-session-unavailable",
                "the observed process session could not be read",
            ));
        }
        Ok(session_id)
    }

    fn architecture(&self) -> Result<ProcessArchitecture, StructuredServiceError> {
        let mut process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        let mut native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        if unsafe { IsWow64Process2(self.0, &mut process_machine, &mut native_machine) } == 0 {
            return Err(last_error(
                "process-architecture-unavailable",
                "the observed process architecture could not be read",
            ));
        }
        classify_process_architecture(process_machine, native_machine)
    }

    fn is_protected(&self) -> bool {
        let mut information = PROCESS_PROTECTION_LEVEL_INFORMATION::default();
        if unsafe {
            GetProcessInformation(
                self.0,
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

    fn must_skip_injection(&self) -> bool {
        let mut critical = 0;
        if unsafe { IsProcessCritical(self.0, &mut critical) } == 0 || critical != 0 {
            return true;
        }
        self.image_name().as_deref().map_or(true, |name| {
            is_important_windows_process(name) || is_installer_control_process(name)
        })
    }

    fn image_name(&self) -> Option<String> {
        let mut buffer = vec![0u16; 32_768];
        let mut length = buffer.len() as u32;
        if unsafe {
            QueryFullProcessImageNameW(self.0, PROCESS_NAME_WIN32, buffer.as_mut_ptr(), &mut length)
        } == 0
        {
            return None;
        }
        let path = OsString::from_wide(&buffer[..length as usize]);
        std::path::Path::new(&path)
            .file_name()
            .map(|name| name.to_string_lossy().to_ascii_lowercase())
    }
}

fn classify_process_architecture(
    process_machine: u16,
    native_machine: u16,
) -> Result<ProcessArchitecture, StructuredServiceError> {
    match (process_machine, native_machine) {
        (IMAGE_FILE_MACHINE_I386, _) | (IMAGE_FILE_MACHINE_UNKNOWN, IMAGE_FILE_MACHINE_I386) => {
            Ok(ProcessArchitecture::X86)
        }
        (IMAGE_FILE_MACHINE_AMD64, _) | (IMAGE_FILE_MACHINE_UNKNOWN, IMAGE_FILE_MACHINE_AMD64) => {
            Ok(ProcessArchitecture::X64)
        }
        _ => Err(service_error(
            "process-architecture-unsupported",
            "the observed process architecture has no compatible helper",
            None,
        )),
    }
}

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}

fn is_important_windows_process(name: &str) -> bool {
    matches!(
        name,
        "smss.exe"
            | "csrss.exe"
            | "wininit.exe"
            | "winlogon.exe"
            | "services.exe"
            | "lsass.exe"
            | "fontdrvhost.exe"
    )
}

fn is_installer_control_process(name: &str) -> bool {
    name == "mactype-service-setup.exe" || is_inno_uninstaller(name)
}

fn is_inno_uninstaller(name: &str) -> bool {
    let Some((stem, extension)) = name.rsplit_once('.') else {
        return false;
    };
    if !matches!(extension, "exe" | "tmp") {
        return false;
    }
    let stem = stem.strip_prefix('_').unwrap_or(stem);
    stem.strip_prefix("unins")
        .is_some_and(|sequence| sequence.bytes().all(|character| character.is_ascii_digit()))
}

fn last_error(code: &str, message: &str) -> StructuredServiceError {
    service_error(
        code,
        message,
        std::io::Error::last_os_error().raw_os_error(),
    )
}

fn service_error(code: &str, message: &str, win32_error: Option<i32>) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error: win32_error.map(|code| code as u32),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows_sys::Win32::System::SystemInformation::IMAGE_FILE_MACHINE_ARM64;

    #[test]
    fn native_arm64_is_not_sent_to_the_x64_helper() {
        assert_eq!(
            classify_process_architecture(IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_ARM64)
                .unwrap(),
            ProcessArchitecture::X64
        );
        let error =
            classify_process_architecture(IMAGE_FILE_MACHINE_UNKNOWN, IMAGE_FILE_MACHINE_ARM64)
                .expect_err("native ARM64 has no fixed compatible helper");
        assert_eq!(error.code, "process-architecture-unsupported");
    }

    #[test]
    fn installer_control_processes_are_never_injection_targets() {
        for name in [
            "mactype-service-setup.exe",
            "unins000.exe",
            "unins000.tmp",
            "_unins.tmp",
            "_unins001.exe",
            "_unins001.tmp",
        ] {
            assert!(
                is_installer_control_process(name),
                "installer control process was eligible for injection: {name}"
            );
            assert!(
                !is_important_windows_process(name),
                "installer control process leaked into the Windows system-process predicate: {name}"
            );
        }

        for name in [
            "mactype-service-setup.exe.disabled",
            "uninstall-helper.exe",
            "unison.exe",
        ] {
            assert!(
                !is_installer_control_process(name),
                "unrelated process was excluded by an over-broad name rule: {name}"
            );
        }

        assert!(is_important_windows_process("services.exe"));
        assert!(!is_installer_control_process("services.exe"));
    }
}
