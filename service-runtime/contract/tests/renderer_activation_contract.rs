use std::mem::{align_of, offset_of, size_of};

use mactype_service_contract::{
    ProfileDigest, RendererActivationContractError, RendererActivationDisposition,
    RendererActivationEvidenceV1, RendererActivationReason, RendererArchitecture,
    RendererCapability, RendererCapabilitySet, RendererModuleLoad, RendererProcessIdentity,
    RendererRuntimeBinding, RuntimeGenerationId, PROFILE_DIGEST_TEXT_BYTES,
    RENDERER_ACTIVATION_EVIDENCE_V1_SIZE, RENDERER_ACTIVATION_QUERY_EXPORT,
    RENDERER_ACTIVATION_SCHEMA_VERSION, RENDERER_CAPABILITY_KNOWN_MASK,
    RUNTIME_GENERATION_TEXT_BYTES,
};

fn binding() -> RendererRuntimeBinding {
    RendererRuntimeBinding::new(
        RuntimeGenerationId::parse(
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        )
        .unwrap(),
        ProfileDigest::parse(
            "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        )
        .unwrap(),
    )
}

fn request() -> RendererActivationEvidenceV1 {
    RendererActivationEvidenceV1::request(
        RendererProcessIdentity {
            pid: 42,
            creation_time: 133_967_890_123_456_789,
            session_id: 2,
            architecture: RendererArchitecture::X64,
        },
        binding(),
        RendererModuleLoad::LoadedByRequest,
    )
}

fn active_evidence() -> RendererActivationEvidenceV1 {
    let mut evidence = request();
    evidence.disposition = RendererActivationDisposition::Active as u8;
    evidence.lifecycle_revision = 7;
    evidence.effective_profile_digest = *evidence.binding.profile_digest().as_wire_bytes();
    evidence.capability_active =
        RendererCapability::Gdi.bit() | RendererCapability::DirectWrite.bit();
    evidence.capability_unavailable = RendererCapability::DirectWriteCore.bit();
    evidence
}

#[test]
fn c_abi_size_alignment_and_offsets_are_fixed() {
    assert_eq!(size_of::<RendererRuntimeBinding>(), 137);
    assert_eq!(align_of::<RendererRuntimeBinding>(), 1);
    assert_eq!(size_of::<RendererActivationEvidenceV1>(), 312);
    assert_eq!(align_of::<RendererActivationEvidenceV1>(), 8);
    assert_eq!(RENDERER_ACTIVATION_EVIDENCE_V1_SIZE, 312);

    assert_eq!(offset_of!(RendererActivationEvidenceV1, struct_size), 0);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, schema_version), 4);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, reason), 6);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, architecture), 8);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, module_load), 9);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, disposition), 10);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, pid), 12);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, session_id), 16);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, creation_time), 24);
    assert_eq!(offset_of!(RendererActivationEvidenceV1, binding), 32);
    assert_eq!(
        offset_of!(RendererActivationEvidenceV1, effective_profile_digest),
        169
    );
    assert_eq!(
        offset_of!(RendererActivationEvidenceV1, lifecycle_revision),
        248
    );
    assert_eq!(
        offset_of!(RendererActivationEvidenceV1, capability_active),
        256
    );
    assert_eq!(
        offset_of!(RendererActivationEvidenceV1, capability_unavailable),
        264
    );
    assert_eq!(
        offset_of!(RendererActivationEvidenceV1, capability_failed),
        272
    );
    assert_eq!(offset_of!(RendererActivationEvidenceV1, reserved), 280);
}

#[test]
fn canonical_identifiers_use_fixed_nul_terminated_ascii() {
    let binding = binding();
    assert_eq!(
        binding.runtime_generation_id().as_wire_bytes().len(),
        RUNTIME_GENERATION_TEXT_BYTES
    );
    assert_eq!(
        binding.runtime_generation_id().as_wire_bytes()[RUNTIME_GENERATION_TEXT_BYTES - 1],
        0
    );
    assert_eq!(
        binding.profile_digest().as_wire_bytes().len(),
        PROFILE_DIGEST_TEXT_BYTES
    );
    assert_eq!(
        binding.profile_digest().as_wire_bytes()[PROFILE_DIGEST_TEXT_BYTES - 1],
        0
    );

    for invalid in [
        "abc",
        "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
        "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    ] {
        assert_eq!(
            RuntimeGenerationId::parse(invalid),
            Err(RendererActivationContractError::InvalidRuntimeGenerationId)
        );
    }
    for invalid in [
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "sha512:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
    ] {
        assert_eq!(
            ProfileDigest::parse(invalid),
            Err(RendererActivationContractError::InvalidProfileDigest)
        );
    }
}

#[test]
fn canonical_enum_and_code_mappings_round_trip() {
    assert_eq!(
        RENDERER_ACTIVATION_QUERY_EXPORT,
        "MacTypeQueryActivationEvidenceV1"
    );
    for value in [RendererArchitecture::X86, RendererArchitecture::X64] {
        assert_eq!(RendererArchitecture::from_code(value.code()), Some(value));
        assert_eq!(RendererArchitecture::try_from(value as u8), Ok(value));
    }
    for value in [
        RendererModuleLoad::NotLoaded,
        RendererModuleLoad::LoadedByRequest,
        RendererModuleLoad::AlreadyLoaded,
        RendererModuleLoad::Unknown,
    ] {
        assert_eq!(RendererModuleLoad::from_code(value.code()), Some(value));
        assert_eq!(RendererModuleLoad::try_from(value as u8), Ok(value));
    }
    for value in [
        RendererActivationDisposition::Active,
        RendererActivationDisposition::QuietSkip,
        RendererActivationDisposition::Failed,
    ] {
        assert_eq!(
            RendererActivationDisposition::from_code(value.code()),
            Some(value)
        );
        assert_eq!(
            RendererActivationDisposition::try_from(value as u8),
            Ok(value)
        );
    }
    for value in [
        RendererActivationReason::None,
        RendererActivationReason::ProfileMissing,
        RendererActivationReason::ProfileInvalid,
        RendererActivationReason::ProfileDigestMismatch,
        RendererActivationReason::ProcessExcluded,
        RendererActivationReason::ProcessUnloadRequested,
        RendererActivationReason::RuntimeGenerationMismatch,
        RendererActivationReason::InitializationFailed,
        RendererActivationReason::HookTransactionFailed,
        RendererActivationReason::LifecycleStopping,
        RendererActivationReason::EvidenceUnavailable,
        RendererActivationReason::UnsupportedContract,
        RendererActivationReason::ModuleLoadFailed,
        RendererActivationReason::ModuleUnloadFailed,
    ] {
        assert_eq!(
            RendererActivationReason::from_code(value.code()),
            Some(value)
        );
        assert_eq!(RendererActivationReason::try_from(value as u16), Ok(value));
    }
    for value in [
        RendererCapability::Gdi,
        RendererCapability::ChildInjection,
        RendererCapability::DirectWrite,
        RendererCapability::DirectWriteCore,
        RendererCapability::Direct2d,
        RendererCapability::GdiPlus,
        RendererCapability::FontSubstitution,
    ] {
        assert_eq!(RendererCapability::from_code(value.code()), Some(value));
        assert!(RendererCapabilitySet::from_bits(value.bit())
            .unwrap()
            .contains(value));
    }
    assert_eq!(
        RendererCapabilitySet::from_bits(RENDERER_CAPABILITY_KNOWN_MASK)
            .unwrap()
            .bits(),
        RENDERER_CAPABILITY_KNOWN_MASK
    );
}

#[test]
fn request_and_completed_evidence_round_trip_without_unsafe_reads() {
    let request = request();
    request.validate_request().unwrap();
    let request_wire = request.to_wire_bytes();
    assert_eq!(
        RendererActivationEvidenceV1::request_from_wire_bytes(&request_wire).unwrap(),
        request
    );

    let mut already_loaded = request;
    already_loaded.module_load = RendererModuleLoad::AlreadyLoaded as u8;
    already_loaded.validate_request().unwrap();
    assert_eq!(
        RendererActivationEvidenceV1::request_from_wire_bytes(&already_loaded.to_wire_bytes())
            .unwrap()
            .module_load,
        RendererModuleLoad::AlreadyLoaded as u8
    );

    let active = active_evidence();
    active.validate().unwrap();
    let evidence_wire = active.to_wire_bytes();
    let decoded = RendererActivationEvidenceV1::from_wire_bytes(&evidence_wire).unwrap();
    assert_eq!(decoded, active);
    assert_eq!(decoded.identity().unwrap().pid, 42);
    assert_eq!(
        decoded.disposition().unwrap(),
        RendererActivationDisposition::Active
    );
    assert_eq!(decoded.reason().unwrap(), RendererActivationReason::None);
    assert_eq!(
        decoded.effective_profile_digest().unwrap().unwrap(),
        *decoded.binding.profile_digest()
    );
}

#[test]
fn quiet_skip_and_failure_are_distinct_from_active() {
    let mut active_with_optional_failure = active_evidence();
    active_with_optional_failure.capability_unavailable = 0;
    active_with_optional_failure.capability_failed = RendererCapability::DirectWriteCore.bit();
    active_with_optional_failure.validate().unwrap();

    let mut skipped = request();
    skipped.disposition = RendererActivationDisposition::QuietSkip as u8;
    skipped.reason = RendererActivationReason::ProcessExcluded as u16;
    skipped.lifecycle_revision = 3;
    skipped.effective_profile_digest = *skipped.binding.profile_digest().as_wire_bytes();
    skipped.capability_unavailable = RENDERER_CAPABILITY_KNOWN_MASK;
    skipped.validate().unwrap();

    let mut failed = request();
    failed.disposition = RendererActivationDisposition::Failed as u8;
    failed.reason = RendererActivationReason::ProfileInvalid as u16;
    failed.module_load = RendererModuleLoad::NotLoaded as u8;
    failed.validate().unwrap();

    skipped.capability_unavailable &= !RendererCapability::Gdi.bit();
    skipped.capability_active = RendererCapability::Gdi.bit();
    assert_eq!(
        skipped.validate(),
        Err(RendererActivationContractError::InconsistentEvidence)
    );
}

#[test]
fn malformed_size_version_identity_and_output_are_rejected() {
    let mut malformed = request();
    malformed.struct_size -= 1;
    assert_eq!(
        malformed.validate_request(),
        Err(RendererActivationContractError::InvalidStructSize)
    );

    let mut malformed = request();
    malformed.schema_version = RENDERER_ACTIVATION_SCHEMA_VERSION + 1;
    assert_eq!(
        malformed.validate_request(),
        Err(RendererActivationContractError::UnsupportedVersion)
    );

    let mut malformed = request();
    malformed.pid = 0;
    assert_eq!(
        malformed.validate_request(),
        Err(RendererActivationContractError::InvalidIdentity)
    );

    let mut malformed = request();
    malformed.disposition = RendererActivationDisposition::Active as u8;
    assert_eq!(
        malformed.validate_request(),
        Err(RendererActivationContractError::NonzeroRequestOutput)
    );

    assert_eq!(
        RendererActivationEvidenceV1::request_from_wire_bytes(&[0; 8]),
        Err(RendererActivationContractError::InvalidWireSize)
    );
}

#[test]
fn malformed_enums_reserved_bytes_and_capability_masks_are_rejected() {
    let mut malformed = active_evidence();
    malformed.architecture = 0xff;
    assert_eq!(
        malformed.validate(),
        Err(RendererActivationContractError::InvalidArchitecture)
    );

    let mut malformed = active_evidence();
    malformed.disposition = 0xff;
    assert_eq!(
        malformed.validate(),
        Err(RendererActivationContractError::InvalidActivationDisposition)
    );

    let mut malformed = active_evidence();
    malformed.reason = 0xffff;
    assert_eq!(
        malformed.validate(),
        Err(RendererActivationContractError::InvalidActivationReason)
    );

    let mut malformed = active_evidence();
    malformed.reserved[0] = 1;
    assert_eq!(
        malformed.validate(),
        Err(RendererActivationContractError::NonzeroReserved)
    );

    let mut malformed = active_evidence();
    malformed.capability_active = 1_u64 << 63;
    assert_eq!(
        malformed.validate(),
        Err(RendererActivationContractError::InvalidCapabilityMask)
    );

    let mut malformed = active_evidence();
    malformed.capability_unavailable |= RendererCapability::Gdi.bit();
    assert_eq!(
        malformed.validate(),
        Err(RendererActivationContractError::OverlappingCapabilities)
    );
}

#[test]
fn active_and_quiet_skip_evidence_must_bind_the_effective_profile() {
    let mut malformed = active_evidence();
    let other = ProfileDigest::parse(
        "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    )
    .unwrap();
    malformed.effective_profile_digest = *other.as_wire_bytes();
    assert_eq!(
        malformed.validate(),
        Err(RendererActivationContractError::InconsistentEvidence)
    );

    malformed.effective_profile_digest[0] = b'X';
    assert_eq!(
        malformed.validate(),
        Err(RendererActivationContractError::InvalidProfileDigest)
    );
}
