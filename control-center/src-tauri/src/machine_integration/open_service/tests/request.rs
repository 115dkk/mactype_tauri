use super::super::*;

fn write_complete_installed_package(install_root: &std::path::Path) {
    let payload_root = install_root.join("service-runtime/payload/files");
    std::fs::create_dir_all(&payload_root).unwrap();
    std::fs::write(
        install_root.join("MacType Control Center.exe"),
        b"control-center",
    )
    .unwrap();
    std::fs::write(
        install_root.join("service-runtime/mactype-service-setup.exe"),
        b"setup",
    )
    .unwrap();
    let files = mactype_service_contract::IMMUTABLE_RUNTIME_FILES
        .iter()
        .map(|name| (*name, format!("sha256:{}", "0".repeat(64))))
        .collect::<std::collections::BTreeMap<_, _>>();
    let manifest = serde_json::json!({
        "schema": mactype_service_contract::RUNTIME_MANIFEST_SCHEMA,
        "version": "0.1.0",
        "files": files,
    });
    std::fs::write(
        install_root.join("service-runtime/payload/manifest.json"),
        serde_json::to_vec(&manifest).unwrap(),
    )
    .unwrap();
    for name in mactype_service_contract::IMMUTABLE_RUNTIME_FILES {
        std::fs::write(payload_root.join(name), format!("payload-{name}")).unwrap();
    }
}

#[test]
fn setup_boundary_accepts_only_fixed_verbs() {
    assert_eq!(SystemServiceAction::Install.setup_verb(), Some("install"));
    assert_eq!(
        SystemServiceAction::PublishProfile.setup_verb(),
        Some("publish-profile")
    );
    assert_eq!(SystemServiceAction::RemoveLegacy.setup_verb(), None);
    assert!(SystemServiceAction::RemoveLegacy.needs_profile_input());
}

#[test]
fn missing_registered_installation_requires_the_complete_installer_before_elevation() {
    let program_files = std::path::Path::new(r"C:\Program Files");

    let error = resolve_installed_package_for_trusted_layout(program_files, None).unwrap_err();

    assert!(
        error.starts_with("control-center-installation-required:"),
        "{error}"
    );
}

#[test]
fn installation_preflight_errors_project_distinct_read_only_states() {
    use crate::service_contract::ServiceManagementPackageState;

    assert_eq!(
        management_package_state_from_error(
            "control-center-installation-required: run the installer"
        ),
        ServiceManagementPackageState::NotInstalled
    );
    assert_eq!(
        management_package_state_from_error(
            "control-center-installation-incomplete: runtime is missing"
        ),
        ServiceManagementPackageState::Incomplete
    );
    assert_eq!(
        management_package_state_from_error(
            "control-center-installation-untrusted: outside Program Files"
        ),
        ServiceManagementPackageState::Untrusted
    );
}

#[test]
fn registered_control_center_without_service_runtime_is_an_incomplete_installation() {
    let fixture =
        std::env::temp_dir().join(format!("mactype-incomplete-package-{}", std::process::id()));
    let program_files = fixture.join("Program Files");
    let install_root = program_files.join("MacType");
    std::fs::create_dir_all(&install_root).unwrap();
    std::fs::write(
        install_root.join("MacType Control Center.exe"),
        b"control-center",
    )
    .unwrap();

    let error = resolve_installed_package_for_trusted_layout(&program_files, Some(&install_root))
        .unwrap_err();

    assert!(
        error.starts_with("control-center-installation-incomplete:"),
        "{error}"
    );
    std::fs::remove_dir_all(fixture).unwrap();
}

#[test]
fn complete_registered_package_delegates_to_its_program_files_control_center() {
    let fixture =
        std::env::temp_dir().join(format!("mactype-complete-package-{}", std::process::id()));
    let program_files = fixture.join("Program Files");
    let install_root = program_files.join("MacType");
    write_complete_installed_package(&install_root);

    let resolved =
        resolve_installed_package_for_trusted_layout(&program_files, Some(&install_root)).unwrap();

    assert_eq!(
        resolved,
        std::fs::canonicalize(&install_root)
            .unwrap()
            .join("MacType Control Center.exe")
    );
    std::fs::remove_dir_all(fixture).unwrap();
}

#[test]
fn registered_package_with_a_manifest_but_missing_payload_is_incomplete() {
    let fixture =
        std::env::temp_dir().join(format!("mactype-missing-payload-{}", std::process::id()));
    let program_files = fixture.join("Program Files");
    let install_root = program_files.join("MacType");
    write_complete_installed_package(&install_root);
    std::fs::remove_file(install_root.join("service-runtime/payload/files/mactype-injector64.exe"))
        .unwrap();

    let error = resolve_installed_package_for_trusted_layout(&program_files, Some(&install_root))
        .unwrap_err();

    assert!(
        error.starts_with("control-center-installation-incomplete:"),
        "{error}"
    );
    std::fs::remove_dir_all(fixture).unwrap();
}

#[test]
fn complete_package_registered_outside_program_files_is_untrusted() {
    let fixture =
        std::env::temp_dir().join(format!("mactype-untrusted-package-{}", std::process::id()));
    let program_files = fixture.join("Program Files");
    let install_root = fixture.join("Downloads").join("MacType");
    std::fs::create_dir_all(&program_files).unwrap();
    write_complete_installed_package(&install_root);

    let error = resolve_installed_package_for_trusted_layout(&program_files, Some(&install_root))
        .unwrap_err();

    assert!(
        error.starts_with("control-center-installation-untrusted:"),
        "{error}"
    );
    std::fs::remove_dir_all(fixture).unwrap();
}

#[test]
fn outdated_migration_activation_upgrades_then_explicitly_starts() {
    assert_eq!(
        migration_activation_actions(InstallationState::Outdated).unwrap(),
        [SystemServiceAction::Upgrade, SystemServiceAction::Start]
    );
}

#[test]
fn elevated_broker_requires_one_transfer_token_for_every_action() {
    let executable = OsString::from("control-center.exe");
    assert_eq!(
        privileged_request_from_arguments([executable.clone()]).unwrap(),
        None
    );
    assert!(privileged_request_from_arguments([
        executable.clone(),
        OsString::from(BROKER_SWITCH),
        OsString::from("publish-profile"),
    ])
    .is_err());
    let request = privileged_request_from_arguments([
        executable.clone(),
        OsString::from(BROKER_SWITCH),
        OsString::from("publish-profile"),
        OsString::from("--broker-transfer-v1"),
        OsString::from("4242"),
        OsString::from("00112233445566778899aabbccddeeff"),
    ])
    .unwrap()
    .unwrap();
    assert_eq!(request.action, SystemServiceAction::PublishProfile);
    let transfer = request.transfer;
    assert_eq!(transfer.server_pid, 4242);
    assert_eq!(
        transfer.nonce,
        [
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd,
            0xee, 0xff,
        ]
    );
    let start = privileged_request_from_arguments([
        executable.clone(),
        OsString::from(BROKER_SWITCH),
        OsString::from("start"),
        OsString::from("--broker-transfer-v1"),
        OsString::from("4242"),
        OsString::from("00112233445566778899aabbccddeeff"),
    ])
    .unwrap()
    .unwrap();
    assert_eq!(start.action, SystemServiceAction::Start);
    assert_eq!(start.transfer.server_pid, 4242);
    assert!(privileged_request_from_arguments([
        executable.clone(),
        OsString::from(BROKER_SWITCH),
        OsString::from("unknown"),
    ])
    .is_err());
    assert!(privileged_request_from_arguments([
        executable,
        OsString::from(BROKER_SWITCH),
        OsString::from("publish-profile"),
        OsString::from("--broker-transfer-v1"),
        OsString::from("0"),
        OsString::from("00112233445566778899AABBCCDDEEFF"),
    ])
    .is_err());
}

#[test]
fn legacy_tray_autostart_broker_accepts_only_two_fixed_verbs_with_a_transfer_token() {
    let executable = OsString::from("control-center.exe");
    for (verb, action) in [
        (
            "disable-legacy-tray-autostart",
            SystemServiceAction::DisableLegacyTrayAutostart,
        ),
        (
            "restore-legacy-tray-autostart",
            SystemServiceAction::RestoreLegacyTrayAutostart,
        ),
    ] {
        assert_eq!(action.setup_verb(), None);
        assert!(!action.needs_profile_input());
        let request = privileged_request_from_arguments([
            executable.clone(),
            OsString::from(BROKER_SWITCH),
            OsString::from(verb),
            OsString::from("--broker-transfer-v1"),
            OsString::from("4242"),
            OsString::from("00112233445566778899aabbccddeeff"),
        ])
        .unwrap()
        .unwrap();
        assert_eq!(request.action, action);
        assert_eq!(request.transfer.server_pid, 4242);
        assert!(privileged_request_from_arguments([
            executable.clone(),
            OsString::from(BROKER_SWITCH),
            OsString::from(verb),
            OsString::from(r"HKLM\arbitrary"),
        ])
        .is_err());
    }
}
