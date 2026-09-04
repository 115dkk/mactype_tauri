#![cfg(windows)]

use mactype_service_host::{
    PrivateFreeTypeClassification, ProcessInspector, WindowsProcessInspector,
};
use mactype_service_platform::process_session_id;

#[test]
fn windows_inspector_requeries_creation_time_session_and_architecture_from_the_process() {
    let pid = std::process::id();
    let inspector = WindowsProcessInspector::new();

    let identity = inspector.inspect(pid).unwrap().identity;

    assert_eq!(identity.pid, pid);
    assert!(identity.creation_time > 0);
    assert_eq!(identity.session_id, process_session_id(pid).unwrap());
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
