use super::*;

fn official_configuration(path: &Path) -> ServiceConfiguration {
    owned_service_configuration(path)
}

fn complete_migration_snapshot(path: &Path) -> LegacyScmSnapshot {
    LegacyScmSnapshot {
        presence: ServicePresence::Owned,
        state: ServiceRuntimeState::Running,
        configuration: owned_service_configuration(path),
        extended: ServiceExtendedConfiguration {
            description: Some("legacy renderer".to_owned()),
            failure_actions: FailureActionsConfiguration {
                reset_period_seconds: 30,
                reboot_message: None,
                command: None,
                actions: vec![FailureAction {
                    action_type: 1,
                    delay_ms: 1_000,
                }],
            },
            failure_actions_on_non_crash: true,
            delayed_auto_start: false,
            service_sid_type: 0,
            required_privileges: vec!["SeChangeNotifyPrivilege".to_owned()],
            preshutdown_timeout_ms: 180_000,
            triggers: ServiceTriggerConfiguration::None,
            security_descriptor: SecurityDescriptorSnapshot {
                self_relative: {
                    let mut descriptor = vec![0u8; 20];
                    descriptor[0] = 1;
                    descriptor[2] = 4;
                    descriptor[3] = 128;
                    descriptor
                },
            },
        },
    }
}

#[test]
fn direct_scm_registration_matches_the_owned_service_policy() {
    let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
    let configuration = owned_service_configuration(path);
    assert_eq!(
        classify_configuration(&configuration, path),
        ServicePresence::Owned
    );
    assert_eq!(
        configuration.binary_path,
        r#""C:\Program Files\MacType\MacTray.exe" -service"#
    );
    assert_eq!(configuration.dependencies, ["winmgmt"]);
    assert_eq!(configuration.account, "LocalSystem");
    assert_eq!(configuration.display_name, "MacType");
    assert_eq!(configuration.load_order_group, None);
    assert_eq!(configuration.tag_id, 0);
}

#[test]
fn load_order_group_and_tag_are_part_of_strict_service_ownership() {
    let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");

    let mut grouped = official_configuration(path);
    grouped.load_order_group = Some("NetworkProvider".to_owned());
    assert_eq!(
        classify_configuration(&grouped, path),
        ServicePresence::Foreign
    );

    let mut tagged = official_configuration(path);
    tagged.tag_id = 7;
    assert_eq!(
        classify_configuration(&tagged, path),
        ServicePresence::Foreign
    );
}

#[test]
fn migration_snapshot_contract_preserves_the_scm_display_name() {
    let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
    let mut configuration = owned_service_configuration(path);
    configuration.display_name = "MacType legacy renderer".to_owned();
    let serialized = serde_json::to_vec(&configuration).unwrap();
    let restored: ServiceConfiguration = serde_json::from_slice(&serialized).unwrap();

    assert_eq!(restored.display_name, "MacType legacy renderer");
}

#[test]
fn migration_snapshot_owns_config2_and_security_descriptor_data() {
    let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
    let snapshot = LegacyScmSnapshot {
        presence: ServicePresence::Owned,
        state: ServiceRuntimeState::Running,
        configuration: owned_service_configuration(path),
        extended: ServiceExtendedConfiguration {
            description: Some("legacy renderer".to_owned()),
            failure_actions: FailureActionsConfiguration {
                reset_period_seconds: 30,
                reboot_message: None,
                command: None,
                actions: vec![FailureAction {
                    action_type: 1,
                    delay_ms: 1_000,
                }],
            },
            failure_actions_on_non_crash: true,
            delayed_auto_start: false,
            service_sid_type: 0,
            required_privileges: vec!["SeChangeNotifyPrivilege".to_owned()],
            preshutdown_timeout_ms: 180_000,
            triggers: ServiceTriggerConfiguration::None,
            security_descriptor: SecurityDescriptorSnapshot {
                self_relative: {
                    let mut descriptor = vec![0u8; 20];
                    descriptor[0] = 1;
                    descriptor[2] = 4;
                    descriptor[3] = 128;
                    descriptor
                },
            },
        },
    };

    let serialized = serde_json::to_vec(&snapshot).unwrap();
    let restored: LegacyScmSnapshot = serde_json::from_slice(&serialized).unwrap();

    assert_eq!(restored, snapshot);
    #[cfg(windows)]
    {
        validate_migration_snapshot(&restored).unwrap();
        let mut invalid = restored.clone();
        invalid
            .extended
            .security_descriptor
            .self_relative
            .truncate(4);
        assert!(validate_migration_snapshot(&invalid).is_err());
    }
}

#[test]
fn rollback_verification_compares_core_config2_and_security_exactly() {
    let expected = complete_migration_snapshot(Path::new(r"C:\Program Files\MacType\MacTray.exe"));

    verify_restored_configuration(&expected, &expected.configuration, &expected.extended).unwrap();

    let mut wrong_core = expected.configuration.clone();
    wrong_core.tag_id = 9;
    assert!(verify_restored_configuration(&expected, &wrong_core, &expected.extended).is_err());

    let mut wrong_config2 = expected.extended.clone();
    wrong_config2.preshutdown_timeout_ms += 1;
    assert!(
        verify_restored_configuration(&expected, &expected.configuration, &wrong_config2).is_err()
    );

    let mut wrong_security = expected.extended.clone();
    wrong_security.security_descriptor.self_relative[4] ^= 1;
    assert!(
        verify_restored_configuration(&expected, &expected.configuration, &wrong_security).is_err()
    );
}

#[test]
fn migration_accepts_no_service_triggers_and_rejects_custom_trigger_state() {
    assert_eq!(
        snapshot_trigger_configuration(0, false, false).unwrap(),
        ServiceTriggerConfiguration::None
    );
    assert!(snapshot_trigger_configuration(1, true, false).is_err());
    assert!(snapshot_trigger_configuration(0, true, false).is_err());
    assert!(snapshot_trigger_configuration(0, false, true).is_err());
}

#[test]
fn migration_accepts_only_exact_running_or_stopped_runtime_states() {
    for state in [ServiceRuntimeState::Running, ServiceRuntimeState::Stopped] {
        require_stable_migration_state(state).unwrap();
    }

    for state in [
        ServiceRuntimeState::StartPending,
        ServiceRuntimeState::StopPending,
        ServiceRuntimeState::ContinuePending,
        ServiceRuntimeState::PausePending,
        ServiceRuntimeState::Paused,
        ServiceRuntimeState::Unknown,
    ] {
        assert!(require_stable_migration_state(state).is_err());
    }
}

struct RecordingServiceRestore {
    steps: Vec<ServiceRestoreStep>,
}

impl ServiceConfigurationRestorer for RecordingServiceRestore {
    fn restore(&mut self, step: ServiceRestoreStep) -> Result<(), String> {
        self.steps.push(step);
        if matches!(
            step,
            ServiceRestoreStep::FailureActions | ServiceRestoreStep::SecurityDescriptor
        ) {
            Err("simulated restore failure".to_owned())
        } else {
            Ok(())
        }
    }
}

#[test]
fn rollback_restores_core_then_config2_then_security_and_aggregates_failures() {
    let mut backend = RecordingServiceRestore { steps: Vec::new() };

    let error = perform_service_configuration_restore(&mut backend).unwrap_err();

    assert_eq!(backend.steps, SERVICE_RESTORE_ORDER);
    assert!(error.contains("failure-actions"));
    assert!(error.contains("security-descriptor"));
}

#[test]
fn only_the_verified_mactray_service_is_owned() {
    let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
    assert_eq!(
        classify_configuration(&official_configuration(path), path),
        ServicePresence::Owned
    );
    let mut foreign = official_configuration(path);
    foreign.binary_path = r"C:\Temp\MacTray.exe -service".to_owned();
    assert_eq!(
        classify_configuration(&foreign, path),
        ServicePresence::Foreign
    );
}

#[test]
fn compatible_unquoted_official_service_is_a_warning_not_foreign() {
    let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
    let mut configuration = official_configuration(path);
    configuration.binary_path = format!("{} -service", path.display());
    assert_eq!(
        classify_configuration(&configuration, path),
        ServicePresence::CompatibleUnquoted
    );
    let status = with_capabilities(
        ServicePresence::CompatibleUnquoted,
        ServiceRuntimeState::Stopped,
        Some(configuration.binary_path),
        None,
        true,
        false,
    );
    assert!(status.can_remove);
    assert!(!status.can_stop);
}

#[test]
fn canonicalized_windows_path_matches_the_service_manager_image_path() {
    let canonical = Path::new(r"\\?\C:\Program Files\MacType\MacTray.exe");
    let mut configuration =
        official_configuration(Path::new(r"C:\Program Files\MacType\MacTray.exe"));

    assert_eq!(
        classify_configuration(&configuration, canonical),
        ServicePresence::Owned
    );

    configuration.binary_path = r"C:\Program Files\MacType\MacTray.exe -service".to_owned();
    let presence = classify_configuration(&configuration, canonical);
    assert_eq!(presence, ServicePresence::CompatibleUnquoted);

    let status = with_capabilities(
        presence,
        ServiceRuntimeState::Running,
        Some(configuration.binary_path),
        None,
        true,
        false,
    );
    assert!(!status.can_remove);
    assert!(status.can_stop);
}

#[test]
fn registry_conflict_blocks_legacy_removal_and_stop() {
    let stopped = with_capabilities(
        ServicePresence::Owned,
        ServiceRuntimeState::Stopped,
        None,
        None,
        true,
        true,
    );
    assert!(!stopped.can_remove);
    assert!(!stopped.can_stop);
}

#[test]
fn missing_legacy_binary_allows_stopped_disposal_but_not_running_mutation() {
    let stopped = with_capabilities(
        ServicePresence::Owned,
        ServiceRuntimeState::Stopped,
        Some(r#""C:\Program Files\MacType\MacTray.exe" -service"#.to_owned()),
        None,
        false,
        false,
    );
    assert!(stopped.can_remove);
    assert!(!stopped.can_stop);

    let running = with_capabilities(
        ServicePresence::Owned,
        ServiceRuntimeState::Running,
        Some(r#""C:\Program Files\MacType\MacTray.exe" -service"#.to_owned()),
        None,
        false,
        false,
    );
    assert!(!running.can_remove);
    assert!(!running.can_stop);
}

#[test]
fn scm_ownership_uses_the_fixed_expected_path_not_binary_availability() {
    let expected = Path::new(r"C:\Program Files\MacType\MacTray.exe");
    let configuration = official_configuration(expected);

    let stopped = status_from_configuration(
        &configuration,
        ServiceRuntimeState::Stopped,
        Some(expected),
        false,
        false,
    );

    assert_eq!(stopped.presence, ServicePresence::Owned);
    assert!(!stopped.trusted_binary_available);
    assert!(stopped.can_remove);

    let no_program_files_identity = status_from_configuration(
        &configuration,
        ServiceRuntimeState::Stopped,
        None,
        false,
        false,
    );
    assert_eq!(no_program_files_identity.presence, ServicePresence::Foreign);
    assert!(!no_program_files_identity.can_remove);
}

fn assert_no_mutation(status: &LegacyServiceStatus) {
    assert!(!status.can_remove);
    assert!(!status.can_stop);
}

#[test]
fn unsafe_service_states_never_expose_mutation_capabilities() {
    for presence in [
        ServicePresence::Foreign,
        ServicePresence::DeletePending,
        ServicePresence::Inaccessible,
    ] {
        for state in [
            ServiceRuntimeState::Stopped,
            ServiceRuntimeState::Running,
            ServiceRuntimeState::Unknown,
        ] {
            let status = with_capabilities(presence, state, None, None, true, false);
            assert_no_mutation(&status);
        }
    }
}

#[test]
fn registry_conflict_blocks_every_mutation() {
    for presence in [
        ServicePresence::Absent,
        ServicePresence::Owned,
        ServicePresence::CompatibleUnquoted,
    ] {
        for state in [ServiceRuntimeState::Stopped, ServiceRuntimeState::Running] {
            let status = with_capabilities(presence, state, None, None, true, true);
            assert_no_mutation(&status);
        }
    }
}

#[test]
fn service_metadata_must_match_the_official_configuration() {
    let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");
    let mut configurations = Vec::new();

    let mut wrong_type = official_configuration(path);
    wrong_type.service_type = 0x20;
    configurations.push(wrong_type);

    let mut manual_start = official_configuration(path);
    manual_start.start_type = 3;
    configurations.push(manual_start);

    let mut wrong_error_control = official_configuration(path);
    wrong_error_control.error_control = 0;
    configurations.push(wrong_error_control);

    let mut wrong_account = official_configuration(path);
    wrong_account.account = "LocalService".to_owned();
    configurations.push(wrong_account);

    let mut missing_dependency = official_configuration(path);
    missing_dependency.dependencies.clear();
    configurations.push(missing_dependency);

    let mut extra_argument = official_configuration(path);
    extra_argument.binary_path.push_str(" unexpected");
    configurations.push(extra_argument);

    for configuration in configurations {
        assert_eq!(
            classify_configuration(&configuration, path),
            ServicePresence::Foreign
        );
    }
}

#[test]
fn ownership_requires_exact_display_name_and_exactly_one_winmgmt_dependency() {
    let path = Path::new(r"C:\Program Files\MacType\MacTray.exe");

    let mut wrong_display = official_configuration(path);
    wrong_display.display_name = "MacType Service".to_owned();
    assert_eq!(
        classify_configuration(&wrong_display, path),
        ServicePresence::Foreign
    );

    let mut extra_dependency = official_configuration(path);
    extra_dependency.dependencies.push("RpcSs".to_owned());
    assert_eq!(
        classify_configuration(&extra_dependency, path),
        ServicePresence::Foreign
    );

    let mut duplicate_dependency = official_configuration(path);
    duplicate_dependency.dependencies.push("WINMGMT".to_owned());
    assert_eq!(
        classify_configuration(&duplicate_dependency, path),
        ServicePresence::Foreign
    );
}

#[test]
fn trusted_binary_must_resolve_to_the_exact_program_files_layout() {
    let root = Path::new("program-files");
    assert!(is_trusted_mactray_layout(
        root,
        &root.join("MacType").join("MacTray.exe")
    ));
    assert!(is_trusted_mactray_layout(
        root,
        &root.join("mactype").join("MACTRAY.EXE")
    ));
    assert!(!is_trusted_mactray_layout(
        root,
        &root.join("MacType-old").join("MacTray.exe")
    ));
    assert!(!is_trusted_mactray_layout(
        root,
        &root.join("MacType").join("bin").join("MacTray.exe")
    ));
    assert!(!is_trusted_mactray_layout(
        root,
        &Path::new("other-root").join("MacType").join("MacTray.exe")
    ));
}
