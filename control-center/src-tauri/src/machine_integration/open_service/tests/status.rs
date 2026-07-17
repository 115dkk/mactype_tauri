use super::super::*;
use crate::machine_integration::legacy_mactray::{
    LegacyServiceStatus, ServicePresence, ServiceRuntimeState,
};

#[test]
fn absent_service_never_claims_system_injection() {
    let status = absent_status();
    assert!(!status.system_injection_active(Some("sha256:profile")));
    assert!(status.can_install);
    assert!(!status.can_start);
}

#[test]
fn stale_legacy_migration_requires_a_stopped_service_or_a_restartable_binary() {
    let mut status = LegacyServiceStatus {
        presence: ServicePresence::Owned,
        state: ServiceRuntimeState::Stopped,
        binary_path: None,
        win32_error: None,
        trusted_binary_available: false,
        registry_conflict: false,
        can_remove: true,
        can_stop: false,
    };
    assert!(legacy_migration_available(&status));

    status.state = ServiceRuntimeState::Running;
    assert!(!legacy_migration_available(&status));

    status.trusted_binary_available = true;
    assert!(legacy_migration_available(&status));

    status.state = ServiceRuntimeState::StopPending;
    assert!(!legacy_migration_available(&status));

    status.state = ServiceRuntimeState::Stopped;
    status.registry_conflict = true;
    assert!(!legacy_migration_available(&status));
}

#[test]
fn bundled_manifest_version_drives_outdated_classification() {
    let manifest = br#"{"schema":1,"version":"0.3.0","files":{"MacType.dll":"sha256:0000000000000000000000000000000000000000000000000000000000000000","MacType64.dll":"sha256:0000000000000000000000000000000000000000000000000000000000000000","mactype-injector32.exe":"sha256:0000000000000000000000000000000000000000000000000000000000000000","mactype-injector64.exe":"sha256:0000000000000000000000000000000000000000000000000000000000000000","mactype-service.exe":"sha256:0000000000000000000000000000000000000000000000000000000000000000"}}"#;
    assert_eq!(bundled_runtime_version(manifest).unwrap(), "0.3.0");
    assert!(bundled_runtime_version(br#"{"schema":2,"version":"0.3.0","files":{}}"#).is_err());

    let root = std::path::Path::new(r"C:\Program Files\MacType Control Center\Service");
    let configured = root.join("bin").join("0.2.0").join("mactype-service.exe");
    let pointer = configured.clone();
    let bundled = root.join("bin").join("0.3.0").join("mactype-service.exe");
    assert_eq!(
        classify_owned_installation(&configured, &pointer, &bundled),
        InstallationState::Outdated
    );
    assert_eq!(
        classify_owned_installation(&bundled, &bundled, &bundled),
        InstallationState::Current
    );
}

#[test]
fn status_ownership_rejects_every_core_service_identity_collision() {
    let owned = |error_control, display: &str, group: &str, tag, dependencies_empty| {
        owned_core_service_configuration(&ObservedCoreServiceConfiguration {
            service_type: 0x10,
            start_type: 2,
            error_control,
            account: "LocalSystem",
            display_name: display,
            load_order_group: group,
            tag_id: tag,
            dependencies_empty,
            protected_image: true,
        })
    };
    assert!(owned(1, "MacType Control Center Service", "", 0, true));
    assert!(!owned(0, "MacType Control Center Service", "", 0, true));
    assert!(!owned(1, "Foreign Display", "", 0, true));
    assert!(!owned(
        1,
        "MacType Control Center Service",
        "group",
        1,
        true
    ));
    assert!(!owned(1, "MacType Control Center Service", "", 0, false));
}

#[test]
fn persisted_health_is_diagnostic_only_and_never_revives_stale_ready() {
    let ready = HealthReport::ready(
        "0.2.0",
        Some("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_owned()),
    );
    let mut failed = ready.clone();
    failed.health = mactype_service_contract::HealthState::Failed;
    failed.active_profile_digest = None;
    failed.readiness = mactype_service_contract::ReadinessReport::initializing();
    failed.last_error = Some(mactype_service_contract::StructuredServiceError {
        code: "service-panic".to_owned(),
        message: "panic boundary".to_owned(),
        win32_error: None,
    });
    assert!(failed.validate().is_ok());

    assert!(select_service_health(RuntimeState::Stopped, 0, None, Some(ready.clone())).is_none());
    let stopped_failure =
        select_service_health(RuntimeState::Stopped, 0, None, Some(failed.clone())).unwrap();
    assert!(!stopped_failure.live);
    assert_eq!(
        stopped_failure.report.health,
        mactype_service_contract::HealthState::Failed
    );
    for transitional in [
        RuntimeState::StartPending,
        RuntimeState::StopPending,
        RuntimeState::Paused,
        RuntimeState::Unknown,
    ] {
        assert!(select_service_health(transitional, 0, None, Some(failed.clone())).is_none());
    }
    assert!(select_service_health(RuntimeState::Running, 42, None, Some(ready)).is_none());
    assert!(
        select_service_health(
            RuntimeState::Running,
            42,
            Some(LiveHealthReport {
                server_pid: 42,
                report: failed,
            }),
            None,
        )
        .unwrap()
        .live
    );
}

#[test]
fn live_ready_is_authoritative_only_when_the_pipe_server_pid_matches_scm() {
    let ready = HealthReport::ready(
        "0.2.0",
        Some("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_owned()),
    );

    assert!(select_service_health(
        RuntimeState::Running,
        4242,
        Some(LiveHealthReport {
            server_pid: 7331,
            report: ready.clone(),
        }),
        None,
    )
    .is_none());
    assert!(select_service_health(
        RuntimeState::Running,
        4242,
        Some(LiveHealthReport {
            server_pid: 4242,
            report: ready,
        }),
        None,
    )
    .is_some());
}

#[test]
fn reveal_accepts_only_owned_stable_protected_service_images() {
    let root = std::path::Path::new(r"C:\Program Files\MacType Control Center\Service");
    let binary = root.join("bin").join("0.3.0").join("mactype-service.exe");
    let mut status = absent_status();
    status.backend = ServiceBackend::OpenSource;
    status.installation = InstallationState::Current;
    status.runtime = RuntimeState::Running;
    status.binary_path = Some(format!(r#""{}" --service"#, binary.display()));
    assert_eq!(validated_reveal_binary(root, &status).unwrap(), binary);

    status.runtime = RuntimeState::StartPending;
    assert!(validated_reveal_binary(root, &status).is_err());
    status.runtime = RuntimeState::Running;
    status.backend = ServiceBackend::Foreign;
    assert!(validated_reveal_binary(root, &status).is_err());
    status.backend = ServiceBackend::OpenSource;
    status.binary_path = Some(r#""C:\Users\person\mactype-service.exe" --service"#.to_owned());
    assert!(validated_reveal_binary(root, &status).is_err());
}
