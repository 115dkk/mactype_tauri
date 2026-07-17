#![cfg(feature = "ci-test-adapter")]

#[path = "support/runtime_installer/active.rs"]
mod active_support;
#[path = "support/runtime_installer/core.rs"]
mod support;

use std::fs;
use std::panic::{catch_unwind, AssertUnwindSafe};

use mactype_service_setup::{RuntimeInstaller, SetupError};

use active_support::active_version;
use support::{payload, test_paths};
#[test]
fn failed_health_check_restores_the_previous_runtime_pointer() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    let first = payload(base.path(), "0.2.0", b"service-v2");
    installer
        .deploy_with_health_check(&first, |_| Ok(()))
        .unwrap();

    let second = payload(base.path(), "0.3.0", b"service-v3");
    let result = installer.deploy_with_health_check(&second, |_| {
        Err(SetupError::Runtime("health never became ready".to_owned()))
    });
    assert!(result.is_err());
    assert_eq!(active_version(paths.runtime_pointer()), "0.2.0");
}

#[test]
fn interrupted_first_activation_removes_only_its_exact_owned_pointer() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    let interrupted = catch_unwind(AssertUnwindSafe(|| {
        let _ = installer
            .deploy_with_health_check(&payload(base.path(), "0.2.0", b"service-v2"), |_| {
                panic!("simulated interruption after first runtime pointer activation")
            });
    }));
    assert!(interrupted.is_err());
    assert!(paths.runtime_pointer().is_file());
    assert!(paths.runtime_activation_journal().is_file());

    assert!(installer
        .recover_interrupted_activation()
        .unwrap()
        .is_none());

    assert!(!paths.runtime_pointer().exists());
    assert!(!paths.runtime_activation_journal().exists());
}

#[test]
fn interrupted_first_activation_preserves_a_later_foreign_pointer_and_journal() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    let interrupted = catch_unwind(AssertUnwindSafe(|| {
        let _ = installer
            .deploy_with_health_check(&payload(base.path(), "0.2.0", b"service-v2"), |_| {
                panic!("simulated interruption after first runtime pointer activation")
            });
    }));
    assert!(interrupted.is_err());
    let foreign = br#"{"schema":1,"version":"9.9.9"}"#;
    fs::write(paths.runtime_pointer(), foreign).unwrap();

    let error = installer.recover_interrupted_activation().unwrap_err();

    assert!(matches!(error, SetupError::CleanupUnknown(_)));
    assert_eq!(fs::read(paths.runtime_pointer()).unwrap(), foreign);
    assert!(paths.runtime_activation_journal().is_file());
}

#[test]
fn interrupted_first_activation_preserves_a_later_non_regular_pointer_and_journal() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    let interrupted = catch_unwind(AssertUnwindSafe(|| {
        let _ = installer
            .deploy_with_health_check(&payload(base.path(), "0.2.0", b"service-v2"), |_| {
                panic!("simulated interruption after first runtime pointer activation")
            });
    }));
    assert!(interrupted.is_err());
    fs::remove_file(paths.runtime_pointer()).unwrap();
    fs::create_dir(paths.runtime_pointer()).unwrap();

    let error = installer.recover_interrupted_activation().unwrap_err();

    assert!(matches!(error, SetupError::CleanupUnknown(_)));
    assert!(paths.runtime_pointer().is_dir());
    assert!(paths.runtime_activation_journal().is_file());
}

#[test]
fn activation_failure_reports_operation_and_pointer_rollback_failure_and_keeps_journal() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    let foreign = br#"{"schema":1,"version":"9.9.9"}"#.to_vec();
    let result =
        installer.deploy_with_health_check(&payload(base.path(), "0.2.0", b"service-v2"), |_| {
            fs::write(paths.runtime_pointer(), &foreign).unwrap();
            Err(SetupError::Runtime("primary health failure".to_owned()))
        });

    let message = result.unwrap_err().to_string();

    assert!(message.contains("primary health failure"));
    assert!(message.contains("rollback"));
    assert_eq!(fs::read(paths.runtime_pointer()).unwrap(), foreign);
    assert!(paths.runtime_activation_journal().is_file());
}
