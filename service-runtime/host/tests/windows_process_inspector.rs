#![cfg(windows)]

use mactype_service_host::{
    ImageSubsystem, InspectionEvidence, PrivateFreeTypeClassification, ProcessInspector,
    TargetLifecycle, WindowsProcessInspector,
};
use mactype_service_platform::process_session_id;

#[test]
fn windows_inspector_reports_identity_and_facts_for_the_current_process() {
    let pid = std::process::id();
    let inspector = WindowsProcessInspector::new();

    let inspection = inspector.inspect(pid).unwrap();
    let identity = inspection.identity.clone();

    assert_eq!(identity.pid, pid);
    assert!(identity.creation_time > 0);
    assert_eq!(identity.session_id, process_session_id(pid).unwrap());
    let expected_name = std::env::current_exe()
        .unwrap()
        .file_name()
        .unwrap()
        .to_string_lossy()
        .into_owned();
    assert_eq!(
        inspection.image_name,
        InspectionEvidence::Known(expected_name)
    );
    assert_eq!(inspection.protected, InspectionEvidence::Known(false));
    assert_eq!(inspection.critical, InspectionEvidence::Known(false));
    assert!(!matches!(
        inspection.dynamic_code,
        InspectionEvidence::Known(policy) if policy.explicitly_blocks_hooks()
    ));
    assert!(!matches!(
        inspection.binary_signature,
        InspectionEvidence::Known(policy) if policy.explicitly_blocks_modules()
    ));
    assert_eq!(
        inspector.probe_target_lifecycle(&identity),
        TargetLifecycle::Running
    );
    assert!(inspector.probe_process_age(&identity).is_some());
    assert_eq!(
        inspector.probe_image_subsystem(&identity),
        ImageSubsystem::Console
    );

    let reused = mactype_service_host::ProcessIdentity {
        creation_time: identity.creation_time.wrapping_add(1),
        ..identity
    };
    assert_eq!(inspector.probe_process_age(&reused), None);
    assert_eq!(
        inspector.probe_image_subsystem(&reused),
        ImageSubsystem::Unavailable
    );
}

#[test]
fn windows_inspector_detects_an_explicit_qt_freetype_engine_marker() {
    let marker = std::hint::black_box("windows:fontengine=freetype");
    assert!(!marker.is_empty());
    let inspector = WindowsProcessInspector::new();
    let identity = inspector.inspect(std::process::id()).unwrap().identity;

    assert_eq!(
        inspector.classify_private_freetype_process(&identity),
        PrivateFreeTypeClassification::Detected
    );
}
