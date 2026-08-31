use std::ffi::OsString;
use std::fs::File;
use std::io::{BufReader, Read};
use std::mem::size_of;
use std::os::windows::ffi::OsStringExt;
use std::path::{Path, PathBuf};

use mactype_service_contract::StructuredServiceError;
use windows_sys::Win32::Foundation::{
    CloseHandle, ERROR_INVALID_PARAMETER, FILETIME, HANDLE, STILL_ACTIVE,
};
use windows_sys::Win32::System::RemoteDesktop::ProcessIdToSessionId;
use windows_sys::Win32::System::SystemInformation::{
    IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_UNKNOWN,
};
use windows_sys::Win32::System::SystemServices::{
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY, PROCESS_MITIGATION_DYNAMIC_CODE_POLICY,
};
use windows_sys::Win32::System::Threading::{
    GetExitCodeProcess, GetProcessInformation, GetProcessMitigationPolicy, GetProcessTimes,
    IsProcessCritical, IsWow64Process2, OpenProcess, ProcessDynamicCodePolicy,
    ProcessProtectionLevelInfo, ProcessSignaturePolicy, QueryFullProcessImageNameW,
    PROCESS_NAME_WIN32, PROCESS_PROTECTION_LEVEL_INFORMATION, PROCESS_QUERY_LIMITED_INFORMATION,
    PROTECTION_LEVEL_NONE,
};

use crate::generated_unity_anticheat_catalog::{
    ANTI_CHEAT_TOP_LEVEL_EXACT, ANTI_CHEAT_TOP_LEVEL_PREFIXES,
};
use crate::{
    BinarySignaturePolicy, DynamicCodePolicy, InspectionEvidence, PrivateFreeTypeClassification,
    ProcessArchitecture, ProcessIdentity, ProcessInspection, ProcessInspectionError,
    ProcessInspector, TargetLiveness, UnityProcessClassification,
};

const MAX_UNITY_INSTALLATION_ENTRIES: usize = 4_096;
const MAX_PRIVATE_FREETYPE_IMAGE_BYTES: u64 = 64 * 1024 * 1024;
const QT_FREETYPE_ENGINE_MARKER: &[u8] = b"windows:fontengine=freetype";
const PRIVATE_FREETYPE_SCAN_BYTES: usize = 64 * 1024;

#[derive(Default)]
pub struct WindowsProcessInspector;

impl WindowsProcessInspector {
    pub const fn new() -> Self {
        Self
    }
}

impl ProcessInspector for WindowsProcessInspector {
    fn inspect(&self, pid: u32) -> Result<ProcessInspection, ProcessInspectionError> {
        if pid == 0 {
            return Err(ProcessInspectionError::TargetUnavailable(service_error(
                "process-identity-invalid",
                "process ID zero cannot be inspected",
                None,
            )));
        }
        let process = OwnedHandle::open(pid).map_err(ProcessInspectionError::TargetUnavailable)?;
        let creation_time = process
            .creation_time()
            .map_err(ProcessInspectionError::TargetUnavailable)?;
        let session_id = process
            .session_id(pid)
            .map_err(ProcessInspectionError::TargetUnavailable)?;
        let architecture = process
            .architecture()
            .map_err(ProcessInspectionError::TargetUnavailable)?;
        Ok(ProcessInspection {
            identity: ProcessIdentity {
                pid,
                creation_time,
                session_id,
                architecture,
            },
            image_name: process.image_name(),
            protected: process.protection(),
            critical: process.criticality(),
            dynamic_code: process.dynamic_code_policy(),
            binary_signature: process.binary_signature_policy(),
        })
    }

    fn probe_target_liveness(&self, identity: &ProcessIdentity) -> TargetLiveness {
        probe_windows_target_liveness(identity)
    }

    fn classify_unity_process(&self, identity: &ProcessIdentity) -> UnityProcessClassification {
        let Ok(process) = OwnedHandle::open(identity.pid) else {
            return UnityProcessClassification::Unavailable;
        };
        if process.creation_time().ok() != Some(identity.creation_time) {
            return UnityProcessClassification::Unavailable;
        }
        let Ok(image_path) = process.image_path() else {
            return UnityProcessClassification::Unavailable;
        };
        classify_unity_installation(&image_path)
    }

    fn classify_private_freetype_process(
        &self,
        identity: &ProcessIdentity,
    ) -> PrivateFreeTypeClassification {
        let Ok(process) = OwnedHandle::open(identity.pid) else {
            return PrivateFreeTypeClassification::Unavailable;
        };
        if process.creation_time().ok() != Some(identity.creation_time) {
            return PrivateFreeTypeClassification::Unavailable;
        }
        let Ok(image_path) = process.image_path() else {
            return PrivateFreeTypeClassification::Unavailable;
        };
        classify_private_freetype_installation(&image_path)
    }
}

fn classify_private_freetype_installation(image_path: &Path) -> PrivateFreeTypeClassification {
    let Ok(metadata) = std::fs::metadata(image_path) else {
        return PrivateFreeTypeClassification::Unavailable;
    };
    if !metadata.is_file()
        || metadata.len() == 0
        || metadata.len() > MAX_PRIVATE_FREETYPE_IMAGE_BYTES
    {
        return PrivateFreeTypeClassification::Unavailable;
    }
    let Ok(detected) = image_contains_private_freetype_marker(image_path) else {
        return PrivateFreeTypeClassification::Unavailable;
    };
    if detected {
        PrivateFreeTypeClassification::Detected
    } else {
        PrivateFreeTypeClassification::NotDetected
    }
}

fn image_contains_private_freetype_marker(image_path: &Path) -> std::io::Result<bool> {
    let utf16_marker: Vec<u8> = QT_FREETYPE_ENGINE_MARKER
        .iter()
        .flat_map(|byte| [*byte, 0])
        .collect();
    let overlap = utf16_marker.len() - 1;
    let mut reader = BufReader::new(File::open(image_path)?);
    let mut buffer = vec![0_u8; PRIVATE_FREETYPE_SCAN_BYTES + overlap];
    let mut retained = 0;
    loop {
        let read = reader.read(&mut buffer[retained..])?;
        if read == 0 {
            return Ok(false);
        }
        let used = retained + read;
        let bytes = &buffer[..used];
        if bytes
            .windows(QT_FREETYPE_ENGINE_MARKER.len())
            .any(|window| window.eq_ignore_ascii_case(QT_FREETYPE_ENGINE_MARKER))
            || bytes
                .windows(utf16_marker.len())
                .any(|window| window.eq_ignore_ascii_case(&utf16_marker))
        {
            return Ok(true);
        }
        retained = overlap.min(used);
        buffer.copy_within(used - retained..used, 0);
    }
}

fn classify_unity_installation(image_path: &Path) -> UnityProcessClassification {
    let Some(directory) = image_path.parent() else {
        return UnityProcessClassification::Unavailable;
    };
    match std::fs::metadata(directory.join("UnityPlayer.dll")) {
        Ok(metadata) if metadata.is_file() => {}
        Ok(_) => return UnityProcessClassification::Unavailable,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return UnityProcessClassification::NotUnity;
        }
        Err(_) => return UnityProcessClassification::Unavailable,
    }

    let Ok(entries) = std::fs::read_dir(directory) else {
        return UnityProcessClassification::Unavailable;
    };
    for (index, entry) in entries.enumerate() {
        if index >= MAX_UNITY_INSTALLATION_ENTRIES {
            return UnityProcessClassification::Unavailable;
        }
        let Ok(entry) = entry else {
            return UnityProcessClassification::Unavailable;
        };
        let name = entry.file_name().to_string_lossy().to_lowercase();
        if ANTI_CHEAT_TOP_LEVEL_EXACT.contains(&name.as_str())
            || ANTI_CHEAT_TOP_LEVEL_PREFIXES
                .iter()
                .any(|prefix| name.starts_with(prefix))
        {
            return UnityProcessClassification::UnityWithAntiCheat;
        }
    }
    UnityProcessClassification::Unity
}

fn probe_windows_target_liveness(identity: &ProcessIdentity) -> TargetLiveness {
    let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, identity.pid) };
    if handle.is_null() {
        // A PID that has left the process table fails to open with
        // ERROR_INVALID_PARAMETER; every other failure (such as access
        // denied) leaves liveness undetermined.
        return if std::io::Error::last_os_error().raw_os_error()
            == Some(ERROR_INVALID_PARAMETER as i32)
        {
            TargetLiveness::Vanished
        } else {
            TargetLiveness::Unknown
        };
    }
    let process = OwnedHandle(handle);
    match process.creation_time() {
        // The PID was reused by a different process; the verified target is gone.
        Ok(creation_time) if creation_time != identity.creation_time => TargetLiveness::Vanished,
        Ok(_) => match process.has_exited() {
            // An open handle can keep an exited process object observable.
            Some(true) => TargetLiveness::Vanished,
            Some(false) => TargetLiveness::Alive,
            None => TargetLiveness::Unknown,
        },
        Err(_) => TargetLiveness::Unknown,
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

    fn has_exited(&self) -> Option<bool> {
        let mut exit_code = 0_u32;
        if unsafe { GetExitCodeProcess(self.0, &mut exit_code) } == 0 {
            return None;
        }
        Some(exit_code != STILL_ACTIVE as u32)
    }

    fn protection(&self) -> InspectionEvidence<bool> {
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
            return InspectionEvidence::Unavailable;
        }
        InspectionEvidence::Known(information.ProtectionLevel != PROTECTION_LEVEL_NONE)
    }

    fn criticality(&self) -> InspectionEvidence<bool> {
        let mut critical = 0;
        if unsafe { IsProcessCritical(self.0, &mut critical) } == 0 {
            return InspectionEvidence::Unavailable;
        }
        InspectionEvidence::Known(critical != 0)
    }

    fn dynamic_code_policy(&self) -> InspectionEvidence<DynamicCodePolicy> {
        let mut dynamic_code = PROCESS_MITIGATION_DYNAMIC_CODE_POLICY::default();
        if unsafe {
            GetProcessMitigationPolicy(
                self.0,
                ProcessDynamicCodePolicy,
                (&mut dynamic_code as *mut PROCESS_MITIGATION_DYNAMIC_CODE_POLICY).cast(),
                size_of::<PROCESS_MITIGATION_DYNAMIC_CODE_POLICY>(),
            )
        } == 0
        {
            return InspectionEvidence::Unavailable;
        }
        let flags = unsafe { dynamic_code.Anonymous.Flags };
        InspectionEvidence::Known(DynamicCodePolicy {
            prohibit_dynamic_code: flags & (1 << 0) != 0,
            allow_thread_opt_out: flags & (1 << 1) != 0,
        })
    }

    fn binary_signature_policy(&self) -> InspectionEvidence<BinarySignaturePolicy> {
        let mut signature = PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY::default();
        if unsafe {
            GetProcessMitigationPolicy(
                self.0,
                ProcessSignaturePolicy,
                (&mut signature as *mut PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY).cast(),
                size_of::<PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY>(),
            )
        } == 0
        {
            return InspectionEvidence::Unavailable;
        }
        let flags = unsafe { signature.Anonymous.Flags };
        InspectionEvidence::Known(BinarySignaturePolicy {
            microsoft_signed_only: flags & (1 << 0) != 0,
            store_signed_only: flags & (1 << 1) != 0,
            mitigation_opt_in: flags & (1 << 2) != 0,
        })
    }

    fn image_name(&self) -> InspectionEvidence<String> {
        self.image_path()
            .ok()
            .and_then(|path| {
                path.file_name()
                    .map(|name| name.to_string_lossy().into_owned())
            })
            .map(InspectionEvidence::Known)
            .unwrap_or(InspectionEvidence::Unavailable)
    }

    fn image_path(&self) -> Result<PathBuf, StructuredServiceError> {
        let mut buffer = vec![0u16; 32_768];
        let mut length = buffer.len() as u32;
        if unsafe {
            QueryFullProcessImageNameW(self.0, PROCESS_NAME_WIN32, buffer.as_mut_ptr(), &mut length)
        } == 0
        {
            return Err(last_error(
                "process-image-path-unavailable",
                "the observed process image path could not be read",
            ));
        }
        let path = OsString::from_wide(&buffer[..length as usize]);
        Ok(PathBuf::from(path))
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
    use std::fs;
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
    fn unity_installation_classification_is_scoped_to_the_game_directory() {
        let directory = tempfile::tempdir().unwrap();
        let executable = directory.path().join("game.exe");
        fs::write(&executable, b"game").unwrap();
        assert_eq!(
            classify_unity_installation(&executable),
            UnityProcessClassification::NotUnity
        );

        fs::write(directory.path().join("UnityPlayer.dll"), b"unity").unwrap();
        assert_eq!(
            classify_unity_installation(&executable),
            UnityProcessClassification::Unity
        );

        fs::create_dir(directory.path().join("EasyAntiCheat")).unwrap();
        assert_eq!(
            classify_unity_installation(&executable),
            UnityProcessClassification::UnityWithAntiCheat
        );
    }

    #[test]
    fn generated_anticheat_prefixes_detect_sibling_client_modules() {
        let directory = tempfile::tempdir().unwrap();
        let executable = directory.path().join("game.exe");
        fs::write(&executable, b"game").unwrap();
        fs::write(directory.path().join("UnityPlayer.dll"), b"unity").unwrap();
        fs::write(directory.path().join("BEClient_x64.dll"), b"anti-cheat").unwrap();
        assert_eq!(
            classify_unity_installation(&executable),
            UnityProcessClassification::UnityWithAntiCheat
        );
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn liveness_probe_distinguishes_a_running_process_from_a_reused_pid() {
        let inspector = WindowsProcessInspector::new();
        let own_identity = inspector.inspect(std::process::id()).unwrap().identity;
        assert_eq!(
            probe_windows_target_liveness(&own_identity),
            TargetLiveness::Alive
        );

        let different_creation_time = ProcessIdentity {
            creation_time: own_identity.creation_time.wrapping_add(1),
            ..own_identity
        };
        assert_eq!(
            probe_windows_target_liveness(&different_creation_time),
            TargetLiveness::Vanished
        );
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn liveness_probe_reports_an_exited_child_as_vanished() {
        let mut child = std::process::Command::new("cmd")
            .stdin(std::process::Stdio::piped())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .unwrap();
        let identity = WindowsProcessInspector::new()
            .inspect(child.id())
            .unwrap()
            .identity;
        drop(child.stdin.take());
        child.wait().unwrap();

        assert_eq!(
            probe_windows_target_liveness(&identity),
            TargetLiveness::Vanished
        );
    }
}
