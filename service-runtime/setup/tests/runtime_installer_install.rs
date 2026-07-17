#![cfg(feature = "ci-test-adapter")]

#[path = "support/runtime_installer/active.rs"]
mod active_support;
#[path = "support/runtime_installer/core.rs"]
mod support;

use std::fs;

use mactype_service_contract::SourceMetadata;
use mactype_service_setup::{ProfileStore, RuntimeInstaller};

use active_support::active_version;
use support::{payload, test_paths};
#[test]
fn bootstrap_preflight_refuses_a_pending_runtime_transaction_without_recovering_it() {
    let (_base, paths) = test_paths();
    fs::create_dir_all(paths.runtime_activation_journal().parent().unwrap()).unwrap();
    let pending = b"pending-runtime-transaction";
    fs::write(paths.runtime_activation_journal(), pending).unwrap();
    let installer = RuntimeInstaller::new(paths.clone());

    let error = installer.inspect_current_stable().unwrap_err();

    assert!(error.to_string().contains("runtime transaction is pending"));
    assert_eq!(
        fs::read(paths.runtime_activation_journal()).unwrap(),
        pending
    );
}

#[test]
fn bootstrap_preflight_rejects_a_runtime_that_differs_from_its_protected_receipt() {
    let (base, paths) = test_paths();
    let installer = RuntimeInstaller::new(paths.clone());
    installer
        .deploy_with_health_check(&payload(base.path(), "0.2.0", b"service"), |_| Ok(()))
        .unwrap();
    fs::write(
        paths
            .runtime_versions()
            .join("0.2.0")
            .join("mactype-service.exe"),
        b"tampered-service",
    )
    .unwrap();

    let error = installer.inspect_current_stable().unwrap_err();

    assert!(error.to_string().contains("differs from its receipt"));
}

#[test]
fn profile_published_before_runtime_is_materialized_before_health_check() {
    let (base, paths) = test_paths();
    let bytes = b"[General]\r\nGammaValue=1.25\r\n";
    ProfileStore::new(paths.clone())
        .publish_and_activate(
            bytes,
            SourceMetadata {
                display_name: "profile first".to_owned(),
            },
        )
        .unwrap();

    let payload = payload(base.path(), "0.2.0", b"service-v2");
    RuntimeInstaller::new(paths)
        .deploy_with_health_check(&payload, |service_binary| {
            assert_eq!(
                fs::read(service_binary.parent().unwrap().join("MacType.ini")).unwrap(),
                bytes
            );
            Ok(())
        })
        .unwrap();
}

#[test]
fn verified_runtime_accepts_only_the_five_manifest_files_plus_generated_mactype_ini() {
    let (base, paths) = test_paths();
    let bytes = b"[General]\r\nGammaValue=1.25\r\n";
    ProfileStore::new(paths.clone())
        .publish_and_activate(
            bytes,
            SourceMetadata {
                display_name: "generated config".to_owned(),
            },
        )
        .unwrap();
    let payload = payload(base.path(), "0.2.0", b"service-v2");
    let installer = RuntimeInstaller::new(paths.clone());
    installer
        .deploy_with_health_check(&payload, |_| Ok(()))
        .unwrap();

    installer
        .deploy_with_health_check(&payload, |service_binary| {
            assert_eq!(
                fs::read(service_binary.parent().unwrap().join("MacType.ini")).unwrap(),
                bytes
            );
            Ok(())
        })
        .unwrap();

    fs::write(
        paths
            .runtime_versions()
            .join("0.2.0")
            .join("unexpected.bin"),
        b"unsigned",
    )
    .unwrap();
    assert!(installer
        .deploy_with_health_check(&payload, |_| Ok(()))
        .is_err());
}

#[test]
fn verified_runtime_is_staged_then_activated_under_the_fixed_machine_root() {
    let (base, paths) = test_paths();
    let payload = payload(base.path(), "0.2.0", b"service-v2");
    let installer = RuntimeInstaller::new(paths.clone());

    let installed = installer
        .deploy_with_health_check(&payload, |_service_binary| Ok(()))
        .unwrap();
    assert_eq!(installed.version(), "0.2.0");
    assert_eq!(active_version(paths.runtime_pointer()), "0.2.0");
    assert_eq!(
        fs::read(
            paths
                .runtime_versions()
                .join("0.2.0")
                .join("mactype-service.exe")
        )
        .unwrap(),
        b"service-v2"
    );
}

#[test]
fn deployment_recovers_an_exact_stale_runtime_staging_directory_after_pid_reuse() {
    let (base, paths) = test_paths();
    fs::create_dir_all(paths.runtime_versions()).unwrap();
    let stale = paths
        .runtime_versions()
        .join(format!(".staging-0.2.0-{}", std::process::id()));
    fs::create_dir(&stale).unwrap();
    fs::write(stale.join("mactype-service.exe"), b"partial").unwrap();
    let unrelated = paths.runtime_versions().join(".staging-0.2.0-not-owned");
    fs::create_dir(&unrelated).unwrap();

    RuntimeInstaller::new(paths)
        .deploy_with_health_check(&payload(base.path(), "0.2.0", b"service-v2"), |_| Ok(()))
        .unwrap();

    assert!(!stale.exists());
    assert!(unrelated.exists());
}

#[test]
fn runtime_staging_cleanup_never_deletes_an_unexpected_entry() {
    let (base, paths) = test_paths();
    let stale = paths
        .runtime_versions()
        .join(format!(".staging-0.2.0-{}", std::process::id()));
    fs::create_dir_all(&stale).unwrap();
    let unexpected = stale.join("do-not-delete.txt");
    fs::write(&unexpected, b"operator file").unwrap();

    assert!(RuntimeInstaller::new(paths)
        .deploy_with_health_check(&payload(base.path(), "0.2.0", b"service-v2"), |_| Ok(()))
        .is_err());

    assert!(unexpected.exists());
}
