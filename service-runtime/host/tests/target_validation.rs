use mactype_service_contract::StructuredServiceError;
use mactype_service_host::{
    BinarySignaturePolicy, DynamicCodePolicy, InspectionEvidence, ProcessArchitecture,
    ProcessIdentity, ProcessInspection, ProcessInspectionError, ProcessInspector,
    ProcessSkipReason, ProcessTargetDecision, ProcessTargetValidator,
};
use std::sync::atomic::{AtomicBool, Ordering};

struct FixedInspector(Result<ProcessInspection, ProcessInspectionError>);

impl ProcessInspector for FixedInspector {
    fn inspect(&self, _pid: u32) -> Result<ProcessInspection, ProcessInspectionError> {
        self.0.clone()
    }
}

fn identity(pid: u32) -> ProcessIdentity {
    ProcessIdentity {
        pid,
        creation_time: 100,
        session_id: 2,
        architecture: ProcessArchitecture::X64,
    }
}

fn ordinary_inspection(pid: u32) -> ProcessInspection {
    ProcessInspection {
        identity: identity(pid),
        image_name: InspectionEvidence::Known("ordinary.exe".to_owned()),
        protected: InspectionEvidence::Known(false),
        critical: InspectionEvidence::Known(false),
        dynamic_code: InspectionEvidence::Known(DynamicCodePolicy {
            prohibit_dynamic_code: false,
            allow_thread_opt_out: false,
        }),
        binary_signature: InspectionEvidence::Known(BinarySignaturePolicy {
            microsoft_signed_only: false,
            store_signed_only: false,
            mitigation_opt_in: false,
        }),
    }
}

#[test]
fn validator_returns_only_verified_eligible_identity() {
    let inspector = FixedInspector(Ok(ordinary_inspection(42)));
    let validator = ProcessTargetValidator::new(900, &inspector);

    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Eligible(identity(42))
    );
}

struct RecordingInspector(AtomicBool);

impl ProcessInspector for RecordingInspector {
    fn inspect(&self, _pid: u32) -> Result<ProcessInspection, ProcessInspectionError> {
        self.0.store(true, Ordering::Relaxed);
        Ok(ordinary_inspection(900))
    }
}

#[test]
fn service_self_is_skipped_before_any_target_query() {
    let inspector = RecordingInspector(AtomicBool::new(false));
    assert_eq!(
        ProcessTargetValidator::new(900, &inspector)
            .validate(900)
            .unwrap(),
        ProcessTargetDecision::Skipped(ProcessSkipReason::ServiceSelf)
    );
    assert!(!inspector.0.load(Ordering::Relaxed));
}

#[test]
fn validator_preserves_the_exact_process_local_skip_reason() {
    let mut session_zero = ordinary_inspection(42);
    session_zero.identity.session_id = 0;
    let mut protected = ordinary_inspection(42);
    protected.protected = InspectionEvidence::Known(true);
    let mut protection_unknown = ordinary_inspection(42);
    protection_unknown.protected = InspectionEvidence::Unavailable;
    let mut critical = ordinary_inspection(42);
    critical.critical = InspectionEvidence::Known(true);
    let mut criticality_unknown = ordinary_inspection(42);
    criticality_unknown.critical = InspectionEvidence::Unavailable;
    let mut image_unknown = ordinary_inspection(42);
    image_unknown.image_name = InspectionEvidence::Unavailable;

    for (candidate, reason) in [
        (session_zero, ProcessSkipReason::SessionZero),
        (protected, ProcessSkipReason::Protected),
        (protection_unknown, ProcessSkipReason::ProtectionUnavailable),
        (critical, ProcessSkipReason::Critical),
        (
            criticality_unknown,
            ProcessSkipReason::CriticalityUnavailable,
        ),
        (image_unknown, ProcessSkipReason::ImageNameUnavailable),
    ] {
        let pid = candidate.identity.pid;
        let inspector = FixedInspector(Ok(candidate));
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(pid)
                .unwrap(),
            ProcessTargetDecision::Skipped(reason)
        );
    }

    let unavailable = FixedInspector(Err(ProcessInspectionError::TargetUnavailable(
        StructuredServiceError {
            code: "process-protected-or-inaccessible".to_owned(),
            message: "target disappeared or cannot be inspected".to_owned(),
            win32_error: Some(5),
        },
    )));
    assert_eq!(
        ProcessTargetValidator::new(900, &unavailable)
            .validate(42)
            .unwrap(),
        ProcessTargetDecision::Skipped(ProcessSkipReason::TargetUnavailable)
    );
}

#[test]
fn validator_rejects_identity_mismatch_and_propagates_infrastructure_failures() {
    let mismatch = FixedInspector(Ok(ordinary_inspection(43)));
    let error = ProcessTargetValidator::new(900, &mismatch)
        .validate(42)
        .unwrap_err();
    assert_eq!(error.code, "process-identity-mismatch");

    let infrastructure = FixedInspector(Err(ProcessInspectionError::Infrastructure(
        StructuredServiceError {
            code: "process-inspector-unavailable".to_owned(),
            message: "inspector initialization failed".to_owned(),
            win32_error: Some(6),
        },
    )));
    let error = ProcessTargetValidator::new(900, &infrastructure)
        .validate(42)
        .unwrap_err();
    assert_eq!(error.code, "process-inspector-unavailable");
}

#[test]
fn exact_system_and_installer_names_are_skipped_without_broad_name_bans() {
    for (name, reason) in [
        ("services.exe", ProcessSkipReason::ImportantWindowsProcess),
        (
            "MACTYPE-SERVICE-SETUP.EXE",
            ProcessSkipReason::InstallerControlProcess,
        ),
        ("unins000.exe", ProcessSkipReason::InstallerControlProcess),
        ("_unins.tmp", ProcessSkipReason::InstallerControlProcess),
        ("_unins001.tmp", ProcessSkipReason::InstallerControlProcess),
    ] {
        let mut inspection = ordinary_inspection(42);
        inspection.image_name = InspectionEvidence::Known(name.to_owned());
        let inspector = FixedInspector(Ok(inspection));
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(42)
                .unwrap(),
            ProcessTargetDecision::Skipped(reason),
            "{name}"
        );
    }

    for name in [
        "mactype-service-setup.exe.disabled",
        "uninstall-helper.exe",
        "unison.exe",
    ] {
        let mut inspection = ordinary_inspection(42);
        inspection.image_name = InspectionEvidence::Known(name.to_owned());
        let inspector = FixedInspector(Ok(inspection));
        assert!(matches!(
            ProcessTargetValidator::new(900, &inspector).validate(42),
            Ok(ProcessTargetDecision::Eligible(_))
        ));
    }
}

#[test]
fn only_known_hook_blocking_mitigations_are_quietly_skipped() {
    let mut dynamic_block = ordinary_inspection(42);
    dynamic_block.dynamic_code = InspectionEvidence::Known(DynamicCodePolicy {
        prohibit_dynamic_code: true,
        allow_thread_opt_out: false,
    });
    let inspector = FixedInspector(Ok(dynamic_block));
    assert_eq!(
        ProcessTargetValidator::new(900, &inspector)
            .validate(42)
            .unwrap(),
        ProcessTargetDecision::Skipped(ProcessSkipReason::DynamicCodeProhibited)
    );

    let mut thread_opt_out = ordinary_inspection(42);
    thread_opt_out.dynamic_code = InspectionEvidence::Known(DynamicCodePolicy {
        prohibit_dynamic_code: true,
        allow_thread_opt_out: true,
    });
    let inspector = FixedInspector(Ok(thread_opt_out));
    assert!(matches!(
        ProcessTargetValidator::new(900, &inspector).validate(42),
        Ok(ProcessTargetDecision::Eligible(_))
    ));

    for signature in [
        BinarySignaturePolicy {
            microsoft_signed_only: true,
            store_signed_only: false,
            mitigation_opt_in: false,
        },
        BinarySignaturePolicy {
            microsoft_signed_only: false,
            store_signed_only: true,
            mitigation_opt_in: false,
        },
        BinarySignaturePolicy {
            microsoft_signed_only: false,
            store_signed_only: false,
            mitigation_opt_in: true,
        },
    ] {
        let mut inspection = ordinary_inspection(42);
        inspection.binary_signature = InspectionEvidence::Known(signature);
        let inspector = FixedInspector(Ok(inspection));
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(42)
                .unwrap(),
            ProcessTargetDecision::Skipped(ProcessSkipReason::BinarySignatureRestricted)
        );
    }

    let mut unknown = ordinary_inspection(42);
    unknown.dynamic_code = InspectionEvidence::Unavailable;
    unknown.binary_signature = InspectionEvidence::Unavailable;
    let inspector = FixedInspector(Ok(unknown));
    assert!(matches!(
        ProcessTargetValidator::new(900, &inspector).validate(42),
        Ok(ProcessTargetDecision::Eligible(_))
    ));
}
