use std::fs::File;
use std::io::{BufReader, Read};
use std::path::Path;

use mactype_service_contract::StructuredServiceError;
use mactype_service_platform::{process_session_id, MachineKind, Process, ProcessAccess};
use windows_sys::Win32::Foundation::ERROR_INVALID_PARAMETER;

use crate::generated_unity_anticheat_catalog::{
    ANTI_CHEAT_TOP_LEVEL_EXACT, ANTI_CHEAT_TOP_LEVEL_PREFIXES,
};
use crate::{
    BinarySignaturePolicy, DynamicCodePolicy, InspectionEvidence, PrivateFreeTypeClassification,
    ProcessArchitecture, ProcessIdentity, ProcessInspection, ProcessInspectionError,
    ProcessInspector, TargetLifecycle, TargetLiveness, UnityProcessClassification,
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
        let process = open_for_identity(pid).map_err(ProcessInspectionError::TargetUnavailable)?;
        let creation_time = process.creation_time().map_err(|error| {
            ProcessInspectionError::TargetUnavailable(os_error(
                "process-creation-time-unavailable",
                "the observed process creation time could not be read",
                &error,
            ))
        })?;
        let session_id = process_session_id(pid).map_err(|error| {
            ProcessInspectionError::TargetUnavailable(os_error(
                "process-session-unavailable",
                "the observed process session could not be read",
                &error,
            ))
        })?;
        let machine = process.machine().map_err(|error| {
            ProcessInspectionError::TargetUnavailable(os_error(
                "process-architecture-unavailable",
                "the observed process architecture could not be read",
                &error,
            ))
        })?;
        let architecture = classify_process_architecture(machine.process, machine.native)
            .map_err(ProcessInspectionError::TargetUnavailable)?;
        Ok(ProcessInspection {
            identity: ProcessIdentity {
                pid,
                creation_time,
                session_id,
                architecture,
            },
            image_name: process
                .image_path_checked()
                .ok()
                .and_then(|path| {
                    path.file_name()
                        .map(|name| name.to_string_lossy().into_owned())
                })
                .map(InspectionEvidence::Known)
                .unwrap_or(InspectionEvidence::Unavailable),
            protected: process
                .protection()
                .map(InspectionEvidence::Known)
                .unwrap_or(InspectionEvidence::Unavailable),
            critical: process
                .criticality()
                .map(InspectionEvidence::Known)
                .unwrap_or(InspectionEvidence::Unavailable),
            dynamic_code: process
                .dynamic_code_mitigation()
                .map(|policy| {
                    InspectionEvidence::Known(DynamicCodePolicy {
                        prohibit_dynamic_code: policy.prohibit_dynamic_code,
                        allow_thread_opt_out: policy.allow_thread_opt_out,
                    })
                })
                .unwrap_or(InspectionEvidence::Unavailable),
            binary_signature: process
                .binary_signature_mitigation()
                .map(|policy| {
                    InspectionEvidence::Known(BinarySignaturePolicy {
                        microsoft_signed_only: policy.microsoft_signed_only,
                        store_signed_only: policy.store_signed_only,
                        mitigation_opt_in: policy.mitigation_opt_in,
                    })
                })
                .unwrap_or(InspectionEvidence::Unavailable),
        })
    }

    fn probe_target_liveness(&self, identity: &ProcessIdentity) -> TargetLiveness {
        probe_windows_target_liveness(identity)
    }

    fn probe_target_lifecycle(&self, identity: &ProcessIdentity) -> TargetLifecycle {
        let process = match Process::open(identity.pid, ProcessAccess::QueryLimited) {
            Ok(process) => process,
            Err(error) => {
                return if error.raw_os_error() == Some(ERROR_INVALID_PARAMETER as i32) {
                    TargetLifecycle::Exiting
                } else {
                    TargetLifecycle::Unknown
                };
            }
        };
        match process.creation_time() {
            // The PID was reused by a different process; the verified target is gone.
            Ok(creation_time) if creation_time != identity.creation_time => {
                return TargetLifecycle::Exiting;
            }
            Ok(_) => {}
            Err(_) => return TargetLifecycle::Unknown,
        }
        let Ok(flags) = process.lifecycle_flags() else {
            return TargetLifecycle::Unknown;
        };
        if flags.deleting {
            TargetLifecycle::Exiting
        } else if flags.frozen {
            TargetLifecycle::Frozen
        } else {
            TargetLifecycle::Running
        }
    }

    fn classify_unity_process(&self, identity: &ProcessIdentity) -> UnityProcessClassification {
        let Ok(process) = Process::open(identity.pid, ProcessAccess::QueryLimited) else {
            return UnityProcessClassification::Unavailable;
        };
        if process.creation_time().ok() != Some(identity.creation_time) {
            return UnityProcessClassification::Unavailable;
        }
        let Ok(image_path) = process.image_path_checked() else {
            return UnityProcessClassification::Unavailable;
        };
        classify_unity_installation(&image_path)
    }

    fn classify_private_freetype_process(
        &self,
        identity: &ProcessIdentity,
    ) -> PrivateFreeTypeClassification {
        let Ok(process) = Process::open(identity.pid, ProcessAccess::QueryLimited) else {
            return PrivateFreeTypeClassification::Unavailable;
        };
        if process.creation_time().ok() != Some(identity.creation_time) {
            return PrivateFreeTypeClassification::Unavailable;
        }
        let Ok(image_path) = process.image_path_checked() else {
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
    let process = match Process::open(identity.pid, ProcessAccess::QueryLimited) {
        Ok(process) => process,
        // A PID that has left the process table fails to open with
        // ERROR_INVALID_PARAMETER; every other failure (such as access
        // denied) leaves liveness undetermined.
        Err(error) => {
            return if error.raw_os_error() == Some(ERROR_INVALID_PARAMETER as i32) {
                TargetLiveness::Vanished
            } else {
                TargetLiveness::Unknown
            };
        }
    };
    match process.creation_time() {
        // The PID was reused by a different process; the verified target is gone.
        Ok(creation_time) if creation_time != identity.creation_time => TargetLiveness::Vanished,
        Ok(_) => match process.exit_code() {
            // An open handle can keep an exited process object observable.
            Ok(Some(_)) => TargetLiveness::Vanished,
            Ok(None) => TargetLiveness::Alive,
            Err(_) => TargetLiveness::Unknown,
        },
        Err(_) => TargetLiveness::Unknown,
    }
}

fn open_for_identity(pid: u32) -> Result<Process, StructuredServiceError> {
    Process::open(pid, ProcessAccess::QueryLimited).map_err(|error| {
        os_error(
            "process-protected-or-inaccessible",
            "the observed process cannot be opened for identity verification",
            &error,
        )
    })
}

fn classify_process_architecture(
    process_machine: MachineKind,
    native_machine: MachineKind,
) -> Result<ProcessArchitecture, StructuredServiceError> {
    match (process_machine, native_machine) {
        (MachineKind::I386, _) | (MachineKind::Unknown, MachineKind::I386) => {
            Ok(ProcessArchitecture::X86)
        }
        (MachineKind::Amd64, _) | (MachineKind::Unknown, MachineKind::Amd64) => {
            Ok(ProcessArchitecture::X64)
        }
        _ => Err(service_error(
            "process-architecture-unsupported",
            "the observed process architecture has no compatible helper",
            None,
        )),
    }
}

/// A structured error carrying the Win32 code of the platform failure.
fn os_error(code: &str, message: &str, error: &std::io::Error) -> StructuredServiceError {
    service_error(code, message, error.raw_os_error())
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
        let arm64 = MachineKind::Other(IMAGE_FILE_MACHINE_ARM64);
        assert_eq!(
            classify_process_architecture(MachineKind::Amd64, arm64).unwrap(),
            ProcessArchitecture::X64
        );
        let error = classify_process_architecture(MachineKind::Unknown, arm64)
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
