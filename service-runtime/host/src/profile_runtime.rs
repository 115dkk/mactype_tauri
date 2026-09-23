#![forbid(unsafe_code)]

use std::io;
use std::path::Path;

use mactype_service_contract::{
    parse_runtime_activation_receipt, validate_protected_renderer_profile, ConsoleProcessPolicy,
    GenerationId, GenerationPointer, MachinePaths, ParsedRuntimeActivationReceipt,
    PrivateFreeTypePolicy, ProfileDigest, RuntimeActivationPhase, RuntimeGenerationPointer,
    StructuredServiceError, UnityFontHookPolicy, MAX_PROFILE_BYTES,
    MAX_RUNTIME_ACTIVATION_RECEIPT_BYTES,
};

use crate::protected_path::{has_reparse_ancestor, read_bounded_regular_file, MAX_POINTER_BYTES};

pub const ACTIVE_PROFILE_ABSENT_CODE: &str = "active-profile-absent";

pub(crate) struct ProtectedProfileSnapshot {
    digest: ProfileDigest,
    bytes: Vec<u8>,
    unity_font_hook: UnityFontHookPolicy,
    private_freetype: PrivateFreeTypePolicy,
    console_process: ConsoleProcessPolicy,
}

impl ProtectedProfileSnapshot {
    pub(crate) fn load(
        paths: &MachinePaths,
        pointer: &GenerationPointer,
        runtime_root: &Path,
    ) -> Result<Self, StructuredServiceError> {
        let profile_path = paths
            .profile_generations()
            .join(pointer.generation().directory_name())
            .join("profile.ini");
        let bytes = read_bounded_protected_file(
            &profile_path,
            MAX_PROFILE_BYTES as u64,
            (
                "active-profile-unavailable",
                "the protected profile generation could not be read",
            ),
            (
                "active-profile-invalid",
                "the protected profile generation is not a bounded regular file",
            ),
        )?;
        validate_protected_renderer_profile(&bytes).map_err(|_| {
            service_error(
                "active-profile-invalid",
                "the protected profile is not a valid self-contained renderer profile",
            )
        })?;
        let calculated = GenerationId::from_profile_bytes(&bytes);
        if &calculated != pointer.generation() {
            return Err(service_error(
                "active-profile-tampered",
                "the protected profile digest does not match its generation",
            ));
        }
        let digest = ProfileDigest::parse(calculated.as_str()).map_err(|_| {
            service_error(
                "active-profile-invalid",
                "the protected profile digest is not canonical",
            )
        })?;
        let unity_font_hook = UnityFontHookPolicy::from_profile_bytes(&bytes);
        let private_freetype = PrivateFreeTypePolicy::from_profile_bytes(&bytes);
        let console_process = ConsoleProcessPolicy::from_profile_bytes(&bytes);
        let snapshot = Self {
            digest,
            bytes,
            unity_font_hook,
            private_freetype,
            console_process,
        };
        snapshot.verify_runtime_copy(runtime_root)?;
        Ok(snapshot)
    }

    pub(crate) const fn digest(&self) -> ProfileDigest {
        self.digest
    }

    pub(crate) const fn unity_font_hook_policy(&self) -> &UnityFontHookPolicy {
        &self.unity_font_hook
    }

    pub(crate) const fn private_freetype_policy(&self) -> PrivateFreeTypePolicy {
        self.private_freetype
    }

    pub(crate) const fn console_process_policy(&self) -> ConsoleProcessPolicy {
        self.console_process
    }

    pub(crate) fn verify_runtime_copy(
        &self,
        runtime_root: &Path,
    ) -> Result<(), StructuredServiceError> {
        let runtime_profile = runtime_root.join("MacType.ini");
        let runtime_bytes = read_bounded_protected_file(
            &runtime_profile,
            MAX_PROFILE_BYTES as u64,
            (
                "runtime-profile-unavailable",
                "the DLL-adjacent generated MacType.ini could not be read",
            ),
            (
                "runtime-profile-invalid",
                "the DLL-adjacent generated MacType.ini is not a bounded regular file",
            ),
        )?;
        if runtime_bytes != self.bytes {
            return Err(service_error(
                "runtime-profile-mismatch",
                "the DLL-adjacent generated MacType.ini does not match the active profile",
            ));
        }
        Ok(())
    }
}

pub(crate) fn ensure_activation_state_is_stable(
    paths: &MachinePaths,
) -> Result<(), StructuredServiceError> {
    if paths.profile_activation_journal().exists() {
        reject_reparse(paths.profile_activation_journal())?;
        return Err(activation_recovery_required());
    }
    validate_runtime_activation_receipt(paths)
}

pub(crate) fn read_active_profile_pointer(
    paths: &MachinePaths,
) -> Result<(Vec<u8>, GenerationPointer), StructuredServiceError> {
    let path = paths.active_profile();
    reject_reparse(path)?;
    let bytes = read_bounded_regular_file(path, MAX_POINTER_BYTES).map_err(|error| {
        if error.kind() == io::ErrorKind::NotFound {
            service_error(
                ACTIVE_PROFILE_ABSENT_CODE,
                "no profile has been published yet, so the service stays stopped until setup publishes one",
            )
        } else if error.kind() == io::ErrorKind::InvalidData {
            service_error(
                "active-profile-invalid",
                "the protected active profile pointer is not a bounded regular file",
            )
        } else {
            service_error(
                "active-profile-unavailable",
                "the protected active profile pointer could not be read",
            )
        }
    })?;
    let pointer = serde_json::from_slice(&bytes).map_err(|_| {
        service_error(
            "active-profile-invalid",
            "the protected active profile pointer is invalid",
        )
    })?;
    Ok((bytes, pointer))
}

pub(crate) fn read_active_runtime_pointer(
    paths: &MachinePaths,
) -> Result<(Vec<u8>, RuntimeGenerationPointer), StructuredServiceError> {
    let bytes = read_bounded_protected_file(
        paths.runtime_pointer(),
        MAX_POINTER_BYTES,
        (
            "active-runtime-unavailable",
            "the protected active runtime pointer could not be read",
        ),
        (
            "active-runtime-invalid",
            "the protected active runtime pointer is not a bounded regular file",
        ),
    )?;
    let pointer = RuntimeGenerationPointer::parse(&bytes).map_err(|_| {
        service_error(
            "active-runtime-invalid",
            "the protected active runtime pointer has an unsupported value",
        )
    })?;
    Ok((bytes, pointer))
}

fn validate_runtime_activation_receipt(paths: &MachinePaths) -> Result<(), StructuredServiceError> {
    let journal_path = paths.runtime_activation_journal();
    if !journal_path.exists() {
        return Ok(());
    }
    let journal_bytes = read_bounded_protected_file(
        journal_path,
        MAX_RUNTIME_ACTIVATION_RECEIPT_BYTES,
        (
            "activation-recovery-required",
            "the runtime activation receipt could not be read",
        ),
        (
            "activation-recovery-required",
            "the runtime activation receipt is not a bounded regular file",
        ),
    )?;
    let ParsedRuntimeActivationReceipt::Current(receipt) =
        parse_runtime_activation_receipt(&journal_bytes)
            .map_err(|_| activation_recovery_required())?
    else {
        return Err(activation_recovery_required());
    };
    if receipt.phase() != RuntimeActivationPhase::Committed {
        return Err(activation_recovery_required());
    }

    let pointer_bytes = read_bounded_protected_file(
        paths.runtime_pointer(),
        MAX_POINTER_BYTES,
        (
            "activation-recovery-required",
            "the active runtime pointer could not be read during activation",
        ),
        (
            "activation-recovery-required",
            "the active runtime pointer is not a bounded regular file during activation",
        ),
    )?;
    let active = RuntimeGenerationPointer::parse(&pointer_bytes)
        .map_err(|_| activation_recovery_required())?;
    if &active != receipt.activated() {
        return Err(activation_recovery_required());
    }
    Ok(())
}

fn activation_recovery_required() -> StructuredServiceError {
    service_error(
        "activation-recovery-required",
        "a protected activation journal requires setup recovery before start unless it durably commits and exactly owns the active runtime candidate",
    )
}

fn read_bounded_protected_file(
    path: &Path,
    maximum_bytes: u64,
    unavailable: (&str, &str),
    invalid: (&str, &str),
) -> Result<Vec<u8>, StructuredServiceError> {
    reject_reparse(path)?;
    read_bounded_regular_file(path, maximum_bytes).map_err(|error| {
        if error.kind() == io::ErrorKind::InvalidData {
            service_error(invalid.0, invalid.1)
        } else {
            service_error(unavailable.0, unavailable.1)
        }
    })
}

fn reject_reparse(path: &Path) -> Result<(), StructuredServiceError> {
    if has_reparse_ancestor(path).map_err(|_| {
        service_error(
            "active-profile-inaccessible",
            "the protected profile path could not be inspected",
        )
    })? {
        return Err(service_error(
            "active-profile-reparse",
            "reparse points are forbidden in the protected profile path",
        ));
    }
    Ok(())
}

fn service_error(code: &str, message: &str) -> StructuredServiceError {
    StructuredServiceError {
        code: code.to_owned(),
        message: message.to_owned(),
        win32_error: None,
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    use mactype_service_contract::MachinePaths;

    use super::ensure_activation_state_is_stable;

    const COMMITTED_RECEIPT_FOR_0_3_0: &[u8] =
        br#"{"schema":3,"phase":"committed","previous":null,"activated":{"schema":1,"version":"0.3.0"}}"#;

    fn machine_paths(root: &Path) -> MachinePaths {
        let program_files = root.join("Program Files");
        let program_data = root.join("ProgramData");
        fs::create_dir_all(&program_files).unwrap();
        fs::create_dir_all(&program_data).unwrap();
        MachinePaths::from_trusted_os_roots(&program_files, &program_data).unwrap()
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn a_committed_receipt_that_does_not_own_the_active_pointer_asks_for_recovery() {
        let root = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
        let paths = machine_paths(root.path());
        fs::create_dir_all(paths.runtime_versions().join("0.2.0")).unwrap();
        fs::create_dir_all(paths.runtime_versions().join("0.3.0")).unwrap();
        fs::write(
            paths.runtime_pointer(),
            br#"{"schema":1,"version":"0.2.0"}"#,
        )
        .unwrap();
        fs::write(
            paths.runtime_activation_journal(),
            COMMITTED_RECEIPT_FOR_0_3_0,
        )
        .unwrap();

        let error = ensure_activation_state_is_stable(&paths)
            .expect_err("the committed receipt does not own the active runtime pointer");

        assert_eq!(error.code, "activation-recovery-required");
    }

    #[test]
    #[cfg_attr(
        all(miri, windows),
        ignore = "Windows Miri does not implement CreateDirectoryW"
    )]
    fn an_unreadable_pointer_under_a_committed_receipt_asks_for_recovery() {
        let root = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
        let paths = machine_paths(root.path());
        fs::create_dir_all(paths.runtime_activation_journal().parent().unwrap()).unwrap();
        fs::write(
            paths.runtime_activation_journal(),
            COMMITTED_RECEIPT_FOR_0_3_0,
        )
        .unwrap();

        let error = ensure_activation_state_is_stable(&paths)
            .expect_err("an absent active runtime pointer cannot own the committed receipt");

        assert_eq!(error.code, "activation-recovery-required");
    }
}
