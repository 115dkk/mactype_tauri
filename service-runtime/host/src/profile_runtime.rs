#![forbid(unsafe_code)]

use std::io;
use std::path::Path;

use mactype_service_contract::{
    parse_runtime_activation_receipt, validate_protected_renderer_profile, GenerationId,
    GenerationPointer, MachinePaths, ParsedRuntimeActivationReceipt, ProfileDigest,
    RuntimeActivationPhase, RuntimeGenerationPointer, StructuredServiceError, MAX_PROFILE_BYTES,
    MAX_RUNTIME_ACTIVATION_RECEIPT_BYTES,
};

use crate::protected_path::{has_reparse_ancestor, read_bounded_regular_file, MAX_POINTER_BYTES};

pub(crate) struct ProtectedProfileSnapshot {
    digest: ProfileDigest,
    bytes: Vec<u8>,
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
        let snapshot = Self { digest, bytes };
        snapshot.verify_runtime_copy(runtime_root)?;
        Ok(snapshot)
    }

    pub(crate) const fn digest(&self) -> ProfileDigest {
        self.digest
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
    let bytes = read_bounded_protected_file(
        paths.active_profile(),
        MAX_POINTER_BYTES,
        (
            "active-profile-unavailable",
            "the protected active profile pointer could not be read",
        ),
        (
            "active-profile-invalid",
            "the protected active profile pointer is not a bounded regular file",
        ),
    )?;
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
