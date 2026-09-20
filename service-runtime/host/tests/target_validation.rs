use mactype_service_contract::StructuredServiceError;
use mactype_service_host::{
    InspectedProcess, ProcessArchitecture, ProcessFacts, ProcessIdentity, ProcessInspector,
    ProcessTargetDecision, ProcessTargetValidator, SkipReason, TargetLifecycle,
};

struct ScriptedInspector {
    inspected: Result<InspectedProcess, StructuredServiceError>,
    lifecycle: TargetLifecycle,
}

impl ProcessInspector for ScriptedInspector {
    fn inspect(&self, _pid: u32) -> Result<InspectedProcess, StructuredServiceError> {
        self.inspected.clone()
    }

    fn probe_target_lifecycle(&self, _identity: &ProcessIdentity) -> TargetLifecycle {
        self.lifecycle
    }
}

fn identity(pid: u32) -> ProcessIdentity {
    ProcessIdentity {
        pid,
        creation_time: 100,
        session_id: 2,
        architecture: ProcessArchitecture::X64,
        protected: false,
    }
}

fn facts() -> ProcessFacts {
    ProcessFacts {
        critical_or_unknown: false,
        prohibits_dynamic_code: false,
        restricts_binary_signature: false,
        image_name: Some("target.exe".to_owned()),
    }
}

fn inspected(identity: ProcessIdentity, facts: ProcessFacts) -> InspectedProcess {
    InspectedProcess { identity, facts }
}

fn inspector(inspected: InspectedProcess) -> ScriptedInspector {
    ScriptedInspector {
        inspected: Ok(inspected),
        lifecycle: TargetLifecycle::Running,
    }
}

#[test]
fn validator_returns_only_verified_eligible_identity() {
    let inspector = inspector(inspected(identity(42), facts()));
    let validator = ProcessTargetValidator::new(900, &inspector);

    assert_eq!(
        validator.validate(42).unwrap(),
        ProcessTargetDecision::Eligible(identity(42))
    );
}

#[test]
fn validator_classifies_every_normal_skip_reason() {
    let mut session_zero = identity(42);
    session_zero.session_id = 0;
    let mut protected = identity(42);
    protected.protected = true;

    let rows = [
        (
            900,
            inspected(identity(900), facts()),
            TargetLifecycle::Running,
            SkipReason::SelfProcess,
        ),
        (
            42,
            inspected(session_zero, facts()),
            TargetLifecycle::Running,
            SkipReason::SessionZero,
        ),
        (
            42,
            inspected(protected, facts()),
            TargetLifecycle::Running,
            SkipReason::Protected,
        ),
        (
            42,
            inspected(
                identity(42),
                ProcessFacts {
                    critical_or_unknown: true,
                    ..facts()
                },
            ),
            TargetLifecycle::Running,
            SkipReason::CriticalOrUnknown,
        ),
        (
            42,
            inspected(
                identity(42),
                ProcessFacts {
                    prohibits_dynamic_code: true,
                    ..facts()
                },
            ),
            TargetLifecycle::Running,
            SkipReason::DynamicCodeMitigation,
        ),
        (
            42,
            inspected(
                identity(42),
                ProcessFacts {
                    restricts_binary_signature: true,
                    ..facts()
                },
            ),
            TargetLifecycle::Running,
            SkipReason::BinarySignatureMitigation,
        ),
        (
            42,
            inspected(
                identity(42),
                ProcessFacts {
                    image_name: Some("services.exe".to_owned()),
                    ..facts()
                },
            ),
            TargetLifecycle::Running,
            SkipReason::ImportantWindowsProcess,
        ),
        (
            42,
            inspected(
                identity(42),
                ProcessFacts {
                    image_name: Some("_unins001.tmp".to_owned()),
                    ..facts()
                },
            ),
            TargetLifecycle::Running,
            SkipReason::InstallerControlProcess,
        ),
        (
            42,
            inspected(
                identity(42),
                ProcessFacts {
                    image_name: None,
                    ..facts()
                },
            ),
            TargetLifecycle::Running,
            SkipReason::ImageNameUnavailable,
        ),
        (
            42,
            inspected(identity(42), facts()),
            TargetLifecycle::Exiting,
            SkipReason::Exiting,
        ),
    ];

    for (pid, inspected, lifecycle, reason) in rows {
        let inspector = ScriptedInspector {
            inspected: Ok(inspected),
            lifecycle,
        };
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(pid)
                .unwrap(),
            ProcessTargetDecision::Skipped(reason)
        );
    }
}

#[test]
fn validator_classifies_target_scoped_inspection_failures() {
    for code in [
        "process-protected-or-inaccessible",
        "process-creation-time-unavailable",
        "process-session-unavailable",
        "process-architecture-unavailable",
        "process-architecture-unsupported",
    ] {
        let inspector = ScriptedInspector {
            inspected: Err(StructuredServiceError {
                code: code.to_owned(),
                message: "target disappeared or cannot be inspected".to_owned(),
                win32_error: Some(5),
            }),
            lifecycle: TargetLifecycle::Running,
        };
        assert_eq!(
            ProcessTargetValidator::new(900, &inspector)
                .validate(42)
                .unwrap(),
            ProcessTargetDecision::Skipped(SkipReason::InspectionFailed(code))
        );
    }
}

#[test]
fn validator_rejects_identity_mismatch_and_propagates_infrastructure_failures() {
    let mismatch = inspector(inspected(identity(43), facts()));
    let error = ProcessTargetValidator::new(900, &mismatch)
        .validate(42)
        .unwrap_err();
    assert_eq!(error.code, "process-identity-mismatch");

    let infrastructure = ScriptedInspector {
        inspected: Err(StructuredServiceError {
            code: "process-inspector-unavailable".to_owned(),
            message: "inspector initialization failed".to_owned(),
            win32_error: Some(6),
        }),
        lifecycle: TargetLifecycle::Running,
    };
    let error = ProcessTargetValidator::new(900, &infrastructure)
        .validate(42)
        .unwrap_err();
    assert_eq!(error.code, "process-inspector-unavailable");
}
