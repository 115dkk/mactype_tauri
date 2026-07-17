use super::super::*;

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
fn setup_broker_rejects_control_center_copies_outside_program_files_layout() {
    let program_files = std::path::Path::new(r"C:\Program Files");
    let copied_executable =
        std::path::Path::new(r"C:\Users\Alice\Downloads\MacType Control Center.exe");

    assert!(setup_path_for_trusted_layout(program_files, copied_executable).is_err());
}

#[test]
fn setup_broker_accepts_only_the_fixed_program_files_target() {
    let program_files = std::path::Path::new(r"C:\Program Files");
    let executable = program_files.join(r"MacType Control Center\MacType Control Center.exe");

    assert_eq!(
        setup_path_for_trusted_layout(program_files, &executable).unwrap(),
        program_files.join(r"MacType Control Center\service-runtime\mactype-service-setup.exe")
    );
}

#[test]
fn outdated_migration_activation_upgrades_then_explicitly_starts() {
    assert_eq!(
        migration_activation_actions(InstallationState::Outdated).unwrap(),
        [SystemServiceAction::Upgrade, SystemServiceAction::Start]
    );
}

#[test]
fn elevated_broker_accepts_transfer_metadata_only_for_profile_actions() {
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
        OsString::from("--profile-transfer-v1"),
        OsString::from("4242"),
        OsString::from("00112233445566778899aabbccddeeff"),
    ])
    .unwrap()
    .unwrap();
    assert_eq!(request.action, SystemServiceAction::PublishProfile);
    let transfer = request.profile_transfer.unwrap();
    assert_eq!(transfer.server_pid, 4242);
    assert_eq!(
        transfer.nonce,
        [
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd,
            0xee, 0xff,
        ]
    );
    assert!(privileged_request_from_arguments([
        executable.clone(),
        OsString::from(BROKER_SWITCH),
        OsString::from("start"),
        OsString::from("--profile-transfer-v1"),
        OsString::from("4242"),
        OsString::from("00112233445566778899aabbccddeeff"),
    ])
    .is_err());
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
        OsString::from("--profile-transfer-v1"),
        OsString::from("0"),
        OsString::from("00112233445566778899AABBCCDDEEFF"),
    ])
    .is_err());
}
