#![cfg(feature = "ci-test-adapter")]

#[path = "support/runtime_installer/active.rs"]
mod active_support;
#[path = "support/runtime_installer/core.rs"]
mod support;

use std::fs;
use std::panic::{catch_unwind, AssertUnwindSafe};

use mactype_service_contract::SourceMetadata;
use mactype_service_setup::{FixedPayload, ProfileStore, RuntimeInstaller, SetupError};

use active_support::active_version;
use support::{payload, test_paths};
#[test]
fn failed_upgrade_restores_a_runtime_whose_adjacent_profile_matches_active_bytes() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    let profile_bytes = b"[General]\r\nGammaValue=1.4\r\n";
    ProfileStore::new(paths.clone())
        .publish_and_activate(
            profile_bytes,
            SourceMetadata {
                display_name: "upgrade rollback".to_owned(),
            },
        )
        .unwrap();
    installer
        .deploy_with_health_check(&payload(base.path(), "0.2.0", b"service-v2"), |_| Ok(()))
        .unwrap();

    let result = installer
        .deploy_with_health_check(&payload(base.path(), "0.3.0", b"service-v3"), |_| {
            Err(SetupError::Runtime("health never became ready".to_owned()))
        });

    assert!(result.is_err());
    assert_eq!(active_version(paths.runtime_pointer()), "0.2.0");
    assert_eq!(
        fs::read(paths.runtime_versions().join("0.2.0").join("MacType.ini")).unwrap(),
        profile_bytes
    );
    assert!(!paths.runtime_activation_journal().exists());
}

#[test]
fn interrupted_upgrade_recovers_the_previous_runtime_and_adjacent_active_profile() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    let profile_bytes = b"[General]\r\nGammaValue=1.4\r\n";
    ProfileStore::new(paths.clone())
        .publish_and_activate(
            profile_bytes,
            SourceMetadata {
                display_name: "power interruption".to_owned(),
            },
        )
        .unwrap();
    installer
        .deploy_with_health_check(&payload(base.path(), "0.2.0", b"service-v2"), |_| Ok(()))
        .unwrap();

    let interrupted = catch_unwind(AssertUnwindSafe(|| {
        let _ = installer
            .deploy_with_health_check(&payload(base.path(), "0.3.0", b"service-v3"), |_| {
                panic!("simulated power interruption after runtime pointer activation")
            });
    }));
    assert!(interrupted.is_err());
    assert_eq!(active_version(paths.runtime_pointer()), "0.3.0");

    let recovered = installer
        .recover_interrupted_activation()
        .unwrap()
        .expect("interrupted upgrade must recover a previous runtime");

    assert_eq!(recovered.version(), "0.2.0");
    assert_eq!(active_version(paths.runtime_pointer()), "0.2.0");
    assert_eq!(
        fs::read(paths.runtime_versions().join("0.2.0").join("MacType.ini")).unwrap(),
        profile_bytes
    );
    assert!(!paths.runtime_activation_journal().exists());
}

#[test]
fn invalid_manifest_never_changes_the_active_runtime() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    let first = payload(base.path(), "0.2.0", b"service-v2");
    installer
        .deploy_with_health_check(&first, |_| Ok(()))
        .unwrap();

    let invalid_root = base.path().join("payload-invalid");
    fs::create_dir_all(invalid_root.join("files")).unwrap();
    fs::write(
        invalid_root.join("files").join("mactype-service.exe"),
        b"tampered",
    )
    .unwrap();
    fs::write(
        invalid_root.join("manifest.json"),
        br#"{"schema":1,"version":"0.3.0","files":{"mactype-service.exe":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}"#,
    )
    .unwrap();
    let invalid = FixedPayload::from_test_root(invalid_root).unwrap();
    assert!(installer
        .deploy_with_health_check(&invalid, |_| Ok(()))
        .is_err());
    assert_eq!(active_version(paths.runtime_pointer()), "0.2.0");
}
