#[path = "support/identity.rs"]
mod identity_support;
#[path = "support/inspector.rs"]
mod inspector_support;

use identity_support::identity as fixed_identity;
use inspector_support::{InspectorResponse, ScriptedInspector};
use mactype_service_contract::{
    ConsoleProcessPolicy, PrivateFreeTypePolicy, StructuredServiceError, UnityFontHookPolicy,
};
use mactype_service_host::{
    BinarySignaturePolicy, DeferralReason, DynamicCodePolicy, ImageSubsystem, InspectionEvidence,
    PrivateFreeTypeClassification, ProcessIdentity, ProcessInspection, ProcessInspectionError,
    ProcessInspector, ProcessSkipReason, ProcessTargetDecision, ProcessTargetValidator,
    TargetLifecycle, UnityProcessClassification,
};
use std::sync::atomic::{AtomicBool, Ordering};

fn identity(pid: u32) -> ProcessIdentity {
    fixed_identity(pid, 100)
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

fn fixed_inspector(inspection: ProcessInspection) -> ScriptedInspector {
    let pid = inspection.identity.pid;
    ScriptedInspector::new([(
        pid,
        InspectorResponse::inspected(inspection, TargetLifecycle::Running),
    )])
}

fn classified_inspector(
    inspection: ProcessInspection,
    response: InspectorResponse,
) -> ScriptedInspector {
    ScriptedInspector::new([(inspection.identity.pid, response)])
}

fn unity_policy(mode: u8) -> UnityFontHookPolicy {
    UnityFontHookPolicy::from_profile_bytes(
        format!("[General]\r\nUnityFontHook={mode}\r\n").as_bytes(),
    )
}

fn console_process_policy(enabled: bool) -> ConsoleProcessPolicy {
    ConsoleProcessPolicy::from_profile_bytes(
        format!(
            "[General]\r\nSkipConsoleProcesses={}\r\n",
            if enabled { 1 } else { 0 }
        )
        .as_bytes(),
    )
}

fn private_freetype_policy(enabled: bool) -> PrivateFreeTypePolicy {
    PrivateFreeTypePolicy::from_profile_bytes(
        format!(
            "[General]\r\nSkipPrivateFreeType={}\r\n",
            if enabled { 1 } else { 0 }
        )
        .as_bytes(),
    )
}

#[test]
fn validator_returns_only_verified_eligible_identity() {
    let inspector = fixed_inspector(ordinary_inspection(42));
    let validator = ProcessTargetValidator::new(900, &inspector);

    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Eligible(identity(42))
    );
}

#[test]
fn validator_defers_frozen_and_quietly_skips_exiting_targets() {
    for (lifecycle, expected) in [
        (
            TargetLifecycle::Frozen,
            ProcessTargetDecision::Deferred {
                identity: identity(42),
                reason: DeferralReason::Frozen,
            },
        ),
        (
            TargetLifecycle::Exiting,
            ProcessTargetDecision::Skipped {
                identity: Some(identity(42)),
                reason: ProcessSkipReason::TargetExiting,
            },
        ),
    ] {
        let inspection = ordinary_inspection(42);
        let inspector =
            ScriptedInspector::new([(42, InspectorResponse::inspected(inspection, lifecycle))]);
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(42)
                .unwrap(),
            expected
        );
    }
}

#[test]
fn enabled_console_policy_quietly_skips_only_detected_console_images() {
    for (classification, expected) in [
        (
            ImageSubsystem::Console,
            ProcessTargetDecision::Skipped {
                identity: Some(identity(42)),
                reason: ProcessSkipReason::ConsoleProcess,
            },
        ),
        (
            ImageSubsystem::Gui,
            ProcessTargetDecision::Eligible(identity(42)),
        ),
        (
            ImageSubsystem::Other,
            ProcessTargetDecision::Eligible(identity(42)),
        ),
        (
            ImageSubsystem::Unavailable,
            ProcessTargetDecision::Eligible(identity(42)),
        ),
    ] {
        let inspection = ordinary_inspection(42);
        let response = InspectorResponse::inspected(inspection.clone(), TargetLifecycle::Running)
            .with_subsystem(classification);
        let inspector = classified_inspector(inspection, response);
        let validator = ProcessTargetValidator::with_profile_policies(
            900,
            &inspector,
            UnityFontHookPolicy::default(),
            PrivateFreeTypePolicy::default(),
            console_process_policy(true),
        );
        assert_eq!(validator.validate(42).unwrap(), expected);
    }
}

#[test]
fn console_policy_runs_after_image_checks_and_before_frozen_lifecycle() {
    let mut missing_image = ordinary_inspection(42);
    missing_image.image_name = InspectionEvidence::Unavailable;
    let response = InspectorResponse::inspected(missing_image.clone(), TargetLifecycle::Frozen)
        .with_subsystem(ImageSubsystem::Console);
    let inspector = classified_inspector(missing_image, response);
    let validator = ProcessTargetValidator::with_profile_policies(
        900,
        &inspector,
        UnityFontHookPolicy::default(),
        PrivateFreeTypePolicy::default(),
        console_process_policy(true),
    );
    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Skipped {
            identity: Some(identity(42)),
            reason: ProcessSkipReason::ImageNameUnavailable,
        }
    );

    let inspection = ordinary_inspection(42);
    let response = InspectorResponse::inspected(inspection.clone(), TargetLifecycle::Frozen)
        .with_subsystem(ImageSubsystem::Console);
    let inspector = classified_inspector(inspection, response);
    let validator = ProcessTargetValidator::with_profile_policies(
        900,
        &inspector,
        UnityFontHookPolicy::default(),
        PrivateFreeTypePolicy::default(),
        console_process_policy(true),
    );
    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Skipped {
            identity: Some(identity(42)),
            reason: ProcessSkipReason::ConsoleProcess,
        }
    );
}

#[test]
fn enabled_private_freetype_policy_quietly_skips_the_detected_process() {
    let inspection = ordinary_inspection(42);
    let response = InspectorResponse::inspected(inspection.clone(), TargetLifecycle::Running)
        .with_private_freetype(PrivateFreeTypeClassification::Detected);
    let inspector = classified_inspector(inspection, response);
    let validator = ProcessTargetValidator::with_profile_policies(
        900,
        &inspector,
        UnityFontHookPolicy::default(),
        private_freetype_policy(true),
        ConsoleProcessPolicy::default(),
    );

    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Skipped {
            identity: Some(identity(42)),
            reason: ProcessSkipReason::PrivateFreeTypeDetected,
        }
    );
}

#[test]
fn private_freetype_skip_does_not_disable_an_enabled_unity_hook_target() {
    let inspection = ordinary_inspection(42);
    let response = InspectorResponse::inspected(inspection.clone(), TargetLifecycle::Running)
        .with_unity(UnityProcessClassification::Unity)
        .with_private_freetype(PrivateFreeTypeClassification::Detected);
    let inspector = classified_inspector(inspection, response);
    let validator = ProcessTargetValidator::with_profile_policies(
        900,
        &inspector,
        unity_policy(2),
        private_freetype_policy(true),
        ConsoleProcessPolicy::default(),
    );

    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Eligible(identity(42))
    );
}

#[test]
fn most_games_mode_quietly_skips_the_entire_anticheat_unity_process() {
    for (classification, reason) in [
        (
            UnityProcessClassification::UnityWithAntiCheat,
            ProcessSkipReason::UnityAntiCheatDetected,
        ),
        (
            UnityProcessClassification::Unavailable,
            ProcessSkipReason::UnitySafetyEvidenceUnavailable,
        ),
    ] {
        let inspection = ordinary_inspection(42);
        let response = InspectorResponse::inspected(inspection.clone(), TargetLifecycle::Running)
            .with_unity(classification);
        let inspector = classified_inspector(inspection, response);
        let validator =
            ProcessTargetValidator::with_unity_font_hook_policy(900, &inspector, unity_policy(2));
        assert_eq!(
            validator.validate(42).unwrap(),
            ProcessTargetDecision::Skipped {
                identity: Some(identity(42)),
                reason,
            }
        );
    }
}

#[test]
fn all_games_mode_does_not_bypass_os_guards_but_does_not_apply_the_anticheat_filter() {
    let inspection = ordinary_inspection(42);
    let response = InspectorResponse::inspected(inspection.clone(), TargetLifecycle::Running)
        .with_unity(UnityProcessClassification::UnityWithAntiCheat);
    let inspector = classified_inspector(inspection, response);
    let validator =
        ProcessTargetValidator::with_unity_font_hook_policy(900, &inspector, unity_policy(3));
    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Eligible(identity(42))
    );

    let mut blocked = ordinary_inspection(42);
    blocked.dynamic_code = InspectionEvidence::Known(DynamicCodePolicy {
        prohibit_dynamic_code: true,
        allow_thread_opt_out: false,
    });
    let response = InspectorResponse::inspected(blocked.clone(), TargetLifecycle::Running)
        .with_unity(UnityProcessClassification::UnityWithAntiCheat);
    let inspector = classified_inspector(blocked, response);
    let validator =
        ProcessTargetValidator::with_unity_font_hook_policy(900, &inspector, unity_policy(3));
    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Skipped {
            identity: Some(identity(42)),
            reason: ProcessSkipReason::DynamicCodeProhibited,
        }
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
        ProcessTargetDecision::Skipped {
            identity: None,
            reason: ProcessSkipReason::ServiceSelf,
        }
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
        let expected_identity = candidate.identity.clone();
        let inspector = fixed_inspector(candidate);
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(pid)
                .unwrap(),
            ProcessTargetDecision::Skipped {
                identity: Some(expected_identity),
                reason,
            }
        );
    }

    let unavailable = ScriptedInspector::new([(
        42,
        InspectorResponse::failure(ProcessInspectionError::TargetUnavailable(
            StructuredServiceError {
                code: "process-protected-or-inaccessible".to_owned(),
                message: "target disappeared or cannot be inspected".to_owned(),
                win32_error: Some(5),
            },
        )),
    )]);
    assert_eq!(
        ProcessTargetValidator::new(900, &unavailable)
            .validate(42)
            .unwrap(),
        ProcessTargetDecision::Skipped {
            identity: None,
            reason: ProcessSkipReason::TargetUnavailable,
        }
    );
}

#[test]
fn validator_rejects_identity_mismatch_and_propagates_infrastructure_failures() {
    let mismatch = ScriptedInspector::new([(
        42,
        InspectorResponse::inspected(ordinary_inspection(43), TargetLifecycle::Running),
    )]);
    let error = ProcessTargetValidator::new(900, &mismatch)
        .validate(42)
        .unwrap_err();
    assert_eq!(error.code, "process-identity-mismatch");

    let infrastructure = ScriptedInspector::new([(
        42,
        InspectorResponse::failure(ProcessInspectionError::Infrastructure(
            StructuredServiceError {
                code: "process-inspector-unavailable".to_owned(),
                message: "inspector initialization failed".to_owned(),
                win32_error: Some(6),
            },
        )),
    )]);
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
        let inspector = fixed_inspector(inspection);
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(42)
                .unwrap(),
            ProcessTargetDecision::Skipped {
                identity: Some(identity(42)),
                reason,
            },
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
        let inspector = fixed_inspector(inspection);
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
    let inspector = fixed_inspector(dynamic_block);
    assert_eq!(
        ProcessTargetValidator::new(900, &inspector)
            .validate(42)
            .unwrap(),
        ProcessTargetDecision::Skipped {
            identity: Some(identity(42)),
            reason: ProcessSkipReason::DynamicCodeProhibited,
        }
    );

    let mut thread_opt_out = ordinary_inspection(42);
    thread_opt_out.dynamic_code = InspectionEvidence::Known(DynamicCodePolicy {
        prohibit_dynamic_code: true,
        allow_thread_opt_out: true,
    });
    let inspector = fixed_inspector(thread_opt_out);
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
        let inspector = fixed_inspector(inspection);
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(42)
                .unwrap(),
            ProcessTargetDecision::Skipped {
                identity: Some(identity(42)),
                reason: ProcessSkipReason::BinarySignatureRestricted,
            }
        );
    }

    let mut unknown = ordinary_inspection(42);
    unknown.dynamic_code = InspectionEvidence::Unavailable;
    unknown.binary_signature = InspectionEvidence::Unavailable;
    let inspector = fixed_inspector(unknown);
    assert!(matches!(
        ProcessTargetValidator::new(900, &inspector).validate(42),
        Ok(ProcessTargetDecision::Eligible(_))
    ));
}
