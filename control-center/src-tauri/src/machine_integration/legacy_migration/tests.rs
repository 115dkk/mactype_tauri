use super::*;

fn status(presence: ServicePresence) -> LegacyServiceStatus {
    LegacyServiceStatus {
        presence,
        state: ServiceRuntimeState::Running,
        binary_path: None,
        win32_error: None,
        trusted_binary_available: true,
        registry_conflict: false,
        can_remove: false,
        can_stop: false,
    }
}

#[test]
fn foreign_legacy_service_is_rejected_before_migration() {
    let error = require_owned_legacy_service(&status(ServicePresence::Foreign)).unwrap_err();

    assert!(error.contains("owned"));
}

#[test]
fn inaccessible_and_delete_pending_services_are_rejected_before_migration() {
    for presence in [
        ServicePresence::Inaccessible,
        ServicePresence::DeletePending,
    ] {
        assert!(require_owned_legacy_service(&status(presence)).is_err());
    }
}

#[test]
fn appinit_conflict_is_rejected_before_migration() {
    let mut legacy = status(ServicePresence::Owned);
    legacy.registry_conflict = true;

    let error = require_owned_legacy_service(&legacy).unwrap_err();

    assert!(error.contains("AppInit"));
}

#[test]
fn missing_mactray_binary_only_allows_a_stopped_owned_service_to_migrate() {
    let mut legacy = status(ServicePresence::Owned);
    legacy.trusted_binary_available = false;

    let error = require_owned_legacy_service(&legacy).unwrap_err();
    assert!(error.contains("running"));
    assert!(error.contains("binary"));

    legacy.state = ServiceRuntimeState::Stopped;
    require_owned_legacy_service(&legacy).unwrap();

    legacy.state = ServiceRuntimeState::StopPending;
    assert!(require_owned_legacy_service(&legacy).is_err());
}

#[test]
fn receipt_rejects_nonempty_load_order_group_or_nonzero_tag() {
    let root = Path::new(r"C:\Program Files\MacType");
    let mut configuration = ServiceConfiguration {
        display_name: "MacType".to_owned(),
        binary_path: r#""C:\Program Files\MacType\MacTray.exe" -service"#.to_owned(),
        service_type: 0x10,
        start_type: 2,
        error_control: 1,
        load_order_group: None,
        tag_id: 0,
        account: "LocalSystem".to_owned(),
        dependencies: vec!["winmgmt".to_owned()],
    };

    validate_service_configuration(root, &configuration).unwrap();
    configuration.load_order_group = Some("NetworkProvider".to_owned());
    assert!(validate_service_configuration(root, &configuration).is_err());
    configuration.load_order_group = None;
    configuration.tag_id = 1;
    assert!(validate_service_configuration(root, &configuration).is_err());
}

#[test]
fn receipt_requires_the_exact_legacy_display_name_and_dependency_set() {
    let root = Path::new(r"C:\Program Files\MacType");
    let mut configuration = ServiceConfiguration {
        display_name: "MacType".to_owned(),
        binary_path: r#""C:\Program Files\MacType\MacTray.exe" -service"#.to_owned(),
        service_type: 0x10,
        start_type: 2,
        error_control: 1,
        load_order_group: None,
        tag_id: 0,
        account: "LocalSystem".to_owned(),
        dependencies: vec!["winmgmt".to_owned()],
    };

    configuration.display_name = "MacType Service".to_owned();
    assert!(validate_service_configuration(root, &configuration).is_err());

    configuration.display_name = "MacType".to_owned();
    configuration.dependencies.push("RpcSs".to_owned());
    assert!(validate_service_configuration(root, &configuration).is_err());
}

#[test]
fn alternative_profile_cannot_traverse_out_of_the_installation() {
    let root = Path::new(r"C:\Program Files\MacType");

    let error = contained_profile_path(root, Path::new(r"..\stolen.ini")).unwrap_err();

    assert!(error.contains("escape"));
}

#[test]
fn reparse_component_is_rejected_before_reading_a_profile() {
    let root = Path::new(r"C:\Program Files\MacType");
    let profile = root.join("ini").join("Community.ini");

    let error = validate_path_chain(root, &profile, |path| Ok(path.ends_with(Path::new("ini"))))
        .unwrap_err();

    assert!(error.contains("reparse"));
}

#[test]
fn modified_backup_is_rejected_by_length_and_sha256() {
    let original = b"[General]\r\nAlternativeFile=ini\\Default.ini\r\n";
    let receipt = BackupFileReceipt {
        role: BackupRole::Configuration,
        original_path: r"C:\Program Files\MacType\MacType.ini".to_owned(),
        backup_file: CONFIGURATION_BACKUP.to_owned(),
        byte_length: original.len() as u64,
        sha256: hex_sha256(original),
    };

    let error = validate_backup_bytes(&receipt, b"[General]\r\nTampered=1\r\n").unwrap_err();

    assert!(error.contains("integrity"));
}

#[test]
fn absent_profile_is_a_versioned_first_class_receipt_state() {
    let receipt = ProfileBackupReceipt::Absent {
        role: BackupRole::ConfigurationAndActiveProfile,
        original_path: r"C:\Program Files\MacType\MacType.ini".to_owned(),
    };

    let encoded = serde_json::to_vec(&receipt).unwrap();
    let decoded: ProfileBackupReceipt = serde_json::from_slice(&encoded).unwrap();

    assert_eq!(decoded.role(), BackupRole::ConfigurationAndActiveProfile);
    assert!(matches!(decoded, ProfileBackupReceipt::Absent { .. }));
}

#[test]
fn rollback_never_deletes_a_profile_that_was_originally_absent() {
    let path = Path::new(r"C:\Program Files\MacType\MacType.ini");

    ensure_absent_restore_target_with(path, |_| Ok(false)).unwrap();
    let error = ensure_absent_restore_target_with(path, |_| Ok(true)).unwrap_err();

    assert!(error.contains("cleanup is unknown"), "{error}");
    assert!(error.contains("refusing to delete"), "{error}");
}

#[test]
fn every_migration_artifact_rejects_a_reparse_point_opened_handle() {
    let root = Path::new(r"C:\ProgramData\MacType\ControlCenter\legacy-migration");
    let generation = root.join("migration-123-456");
    let artifacts = [
        root.join(CURRENT_FILE),
        generation.join(RECEIPT_FILE),
        generation.join(CONFIGURATION_BACKUP),
        generation.join(SERVICE_REGISTRY_EXPORT),
    ];

    for artifact in artifacts {
        let mut opened = false;
        let error = read_bounded_under_with(
            root,
            &artifact,
            MAX_RECEIPT_BYTES,
            |_| Ok(false),
            |_| {
                opened = true;
                Ok((
                    std::io::empty(),
                    OpenedFileMetadata {
                        is_regular_file: true,
                        is_reparse_point: true,
                        byte_length: 0,
                    },
                ))
            },
        )
        .unwrap_err();

        assert!(error.contains("reparse"));
        assert!(
            opened,
            "the final path must be inspected through its handle"
        );
    }
}

#[test]
fn opened_reparse_file_is_rejected_before_the_handle_is_read() {
    struct MustNotRead;

    impl Read for MustNotRead {
        fn read(&mut self, _buffer: &mut [u8]) -> std::io::Result<usize> {
            panic!("a reparse-point handle must never be read")
        }
    }

    let mut opens = 0;
    let error = read_opened_bounded_with(
        Path::new(r"C:\ProgramData\MacType\ControlCenter\legacy-migration\current.json"),
        MAX_RECEIPT_BYTES,
        |_| {
            opens += 1;
            Ok((
                MustNotRead,
                OpenedFileMetadata {
                    is_regular_file: true,
                    is_reparse_point: true,
                    byte_length: 10,
                },
            ))
        },
    )
    .unwrap_err();

    assert_eq!(opens, 1);
    assert!(error.contains("reparse"));
}

#[test]
fn migration_acl_uses_fixed_system32_tool_and_sid_grants() {
    let target = Path::new(r"C:\ProgramData\MacType\ControlCenter\legacy-migration");
    let (program, arguments) = acl_invocation(Path::new(r"C:\Windows\System32"), target);

    assert_eq!(program, PathBuf::from(r"C:\Windows\System32\icacls.exe"));
    assert_eq!(arguments[0], target.as_os_str());
    assert!(arguments.iter().any(|value| value == "*S-1-5-18:(OI)(CI)F"));
    assert!(arguments
        .iter()
        .any(|value| value == "*S-1-5-32-544:(OI)(CI)F"));
    assert!(arguments
        .iter()
        .any(|value| value == "*S-1-5-32-545:(OI)(CI)RX"));
}

#[test]
fn service_registry_export_uses_only_the_fixed_system32_contract() {
    let generation =
        Path::new(r"C:\ProgramData\MacType\ControlCenter\legacy-migration\migration-1-2");
    let export = generation.join(SERVICE_REGISTRY_EXPORT);
    let (program, arguments) =
        registry_export_invocation(Path::new(r"C:\Windows\System32"), generation);

    assert_eq!(program, PathBuf::from(r"C:\Windows\System32\reg.exe"));
    assert_eq!(
        arguments,
        [
            OsString::from("export"),
            OsString::from(r"HKLM\SYSTEM\CurrentControlSet\Services\MacType"),
            export.into_os_string(),
            OsString::from("/y"),
        ]
    );
}

#[test]
fn registry_export_integrity_rejects_empty_tampered_and_oversized_content() {
    let original = b"Windows Registry Editor Version 5.00\r\n";
    let receipt = RegistryExportReceipt {
        export_file: SERVICE_REGISTRY_EXPORT.to_owned(),
        byte_length: original.len() as u64,
        sha256: hex_sha256(original),
    };

    validate_registry_export_bytes(&receipt, original).unwrap();
    assert!(validate_registry_export_bytes(&receipt, b"").is_err());
    assert!(validate_registry_export_bytes(&receipt, b"tampered").is_err());

    let oversized = vec![0u8; MAX_REGISTRY_EXPORT_BYTES as usize + 1];
    let oversized_receipt = RegistryExportReceipt {
        export_file: SERVICE_REGISTRY_EXPORT.to_owned(),
        byte_length: oversized.len() as u64,
        sha256: hex_sha256(&oversized),
    };
    assert!(validate_registry_export_bytes(&oversized_receipt, &oversized).is_err());
}

#[test]
fn acl_failure_prevents_current_pointer_publication() {
    let generation = Path::new(r"C:\ProgramData\MacType\migration-123-456");
    let mut published = false;

    let result = after_hardening_with(
        generation,
        |_| Err("simulated ACL failure".to_owned()),
        |_| {
            published = true;
            Ok(())
        },
    );

    assert!(result.is_err());
    assert!(!published);
}

#[test]
fn registry_export_failure_prevents_current_pointer_publication() {
    let generation = Path::new(r"C:\ProgramData\MacType\migration-123-456");
    let mut published = false;

    let result = after_registry_export_with(
        generation,
        |_| Err("simulated reg.exe failure".to_owned()),
        |_, _| {
            published = true;
            Ok(())
        },
    );

    assert!(result.is_err());
    assert!(!published);
}

#[test]
fn legacy_removal_requires_ready_digest_match_and_valid_backup() {
    for verification in [
        RemovalVerification {
            new_service_ready: false,
            active_digest_match: true,
            backup_valid: true,
        },
        RemovalVerification {
            new_service_ready: true,
            active_digest_match: false,
            backup_valid: true,
        },
        RemovalVerification {
            new_service_ready: true,
            active_digest_match: true,
            backup_valid: false,
        },
    ] {
        assert!(require_removal_verification(verification, true).is_err());
    }

    require_removal_verification(
        RemovalVerification {
            new_service_ready: true,
            active_digest_match: true,
            backup_valid: true,
        },
        true,
    )
    .unwrap();

    let forged_external_result = require_removal_verification(
        RemovalVerification {
            new_service_ready: true,
            active_digest_match: true,
            backup_valid: true,
        },
        false,
    );
    assert!(forged_external_result.is_err());
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum RollbackEvent {
    Stop,
    Profiles,
    Service,
    RunningState,
}

struct RecordingRollback {
    events: Vec<RollbackEvent>,
    fail_at: Option<RollbackEvent>,
}

impl RecordingRollback {
    fn record(&mut self, event: RollbackEvent) -> Result<(), String> {
        self.events.push(event);
        if self.fail_at == Some(event) {
            Err("simulated restore failure".to_owned())
        } else {
            Ok(())
        }
    }
}

impl RollbackBackend for RecordingRollback {
    fn stop_before_restore(&mut self) -> Result<(), String> {
        self.record(RollbackEvent::Stop)
    }

    fn restore_profiles(&mut self) -> Result<(), String> {
        self.record(RollbackEvent::Profiles)
    }

    fn restore_service_configuration(&mut self) -> Result<(), String> {
        self.record(RollbackEvent::Service)
    }

    fn restore_running_state(&mut self) -> Result<(), String> {
        self.record(RollbackEvent::RunningState)
    }
}

#[test]
fn rollback_restores_profiles_before_service_and_never_starts_after_failure() {
    let mut backend = RecordingRollback {
        events: Vec::new(),
        fail_at: Some(RollbackEvent::Service),
    };

    assert!(perform_rollback(&mut backend).is_err());
    assert_eq!(
        backend.events,
        [
            RollbackEvent::Stop,
            RollbackEvent::Profiles,
            RollbackEvent::Service,
        ]
    );
}
