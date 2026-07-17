use std::fs;

use mactype_service_contract::{
    ComponentReadiness, GenerationPointer, MachinePaths, ProfileCatalog, SourceMetadata,
};
use mactype_service_host::{ProtectedProfileInitializer, RuntimeInitializer};

fn paths() -> (tempfile::TempDir, MachinePaths) {
    let base = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
    let program_files = base.path().join("Program Files");
    let program_data = base.path().join("ProgramData");
    fs::create_dir_all(&program_files).unwrap();
    fs::create_dir_all(&program_data).unwrap();
    (
        base,
        MachinePaths::from_trusted_os_roots(&program_files, &program_data).unwrap(),
    )
}

fn install_active_runtime(paths: &MachinePaths, profile_bytes: &[u8]) -> std::path::PathBuf {
    let runtime = paths.runtime_versions().join("0.2.0");
    fs::create_dir_all(&runtime).unwrap();
    fs::write(runtime.join("MacType.ini"), profile_bytes).unwrap();
    fs::create_dir_all(paths.runtime_pointer().parent().unwrap()).unwrap();
    fs::write(
        paths.runtime_pointer(),
        br#"{"schema":1,"version":"0.2.0"}"#,
    )
    .unwrap();
    runtime
}

fn install_active_profile(paths: &MachinePaths, bytes: &[u8]) {
    let mut catalog = ProfileCatalog::new();
    let generation = catalog
        .publish_machine_profile(
            bytes,
            SourceMetadata {
                display_name: "bounded host input".to_owned(),
            },
        )
        .unwrap();
    let directory = paths
        .profile_generations()
        .join(generation.directory_name());
    fs::create_dir_all(&directory).unwrap();
    fs::write(directory.join("profile.ini"), bytes).unwrap();
    fs::create_dir_all(paths.active_profile().parent().unwrap()).unwrap();
    fs::write(
        paths.active_profile(),
        serde_json::to_vec(&GenerationPointer::new(generation)).unwrap(),
    )
    .unwrap();
}

#[test]
fn initializer_rejects_an_oversized_active_profile_pointer_before_parsing() {
    let (_base, paths) = paths();
    fs::create_dir_all(paths.active_profile().parent().unwrap()).unwrap();
    fs::write(paths.active_profile(), vec![b'x'; 64 * 1024 + 1]).unwrap();

    let error = ProtectedProfileInitializer::new(paths)
        .initialize()
        .err()
        .expect("oversized active profile pointer must fail initialization");

    assert_eq!(error.code, "active-profile-invalid");
    assert!(error.message.contains("bounded regular file"));
}

#[test]
fn initializer_rejects_an_oversized_runtime_pointer_before_parsing() {
    let (_base, paths) = paths();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    install_active_profile(&paths, bytes);
    fs::create_dir_all(paths.runtime_pointer().parent().unwrap()).unwrap();
    fs::write(paths.runtime_pointer(), vec![b'x'; 64 * 1024 + 1]).unwrap();

    let error = ProtectedProfileInitializer::new(paths)
        .initialize()
        .err()
        .expect("oversized runtime pointer must fail initialization");

    assert_eq!(error.code, "active-runtime-invalid");
    assert!(error.message.contains("bounded regular file"));
}

#[test]
fn initializer_reports_the_verified_protected_active_profile_digest() {
    let (_base, paths) = paths();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    let mut catalog = ProfileCatalog::new();
    let generation = catalog
        .publish_machine_profile(
            bytes,
            SourceMetadata {
                display_name: "test".to_owned(),
            },
        )
        .unwrap();
    let directory = paths
        .profile_generations()
        .join(generation.directory_name());
    fs::create_dir_all(&directory).unwrap();
    fs::write(directory.join("profile.ini"), bytes).unwrap();
    fs::create_dir_all(paths.active_profile().parent().unwrap()).unwrap();
    fs::write(
        paths.active_profile(),
        serde_json::to_vec(&GenerationPointer::new(generation.clone())).unwrap(),
    )
    .unwrap();
    install_active_runtime(&paths, bytes);

    let initialized = ProtectedProfileInitializer::new(paths.clone())
        .initialize()
        .unwrap();
    assert_eq!(
        initialized.active_profile_digest.as_deref(),
        Some(generation.as_str())
    );
    assert_eq!(initialized.readiness.profile, ComponentReadiness::Ready);
    assert_eq!(
        initialized.readiness.observer,
        ComponentReadiness::NotRequired
    );

    fs::write(
        directory.join("profile.ini"),
        b"[General]\r\nHintingMode=1\r\n",
    )
    .unwrap();
    let error = ProtectedProfileInitializer::new(paths)
        .initialize()
        .err()
        .expect("tampered profile must fail initialization");
    assert_eq!(error.code, "active-profile-tampered");
}

#[test]
fn initializer_rejects_a_dll_adjacent_profile_that_differs_from_the_active_generation() {
    let (_base, paths) = paths();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    let mut catalog = ProfileCatalog::new();
    let generation = catalog
        .publish_machine_profile(
            bytes,
            SourceMetadata {
                display_name: "test".to_owned(),
            },
        )
        .unwrap();
    let directory = paths
        .profile_generations()
        .join(generation.directory_name());
    fs::create_dir_all(&directory).unwrap();
    fs::write(directory.join("profile.ini"), bytes).unwrap();
    fs::create_dir_all(paths.active_profile().parent().unwrap()).unwrap();
    fs::write(
        paths.active_profile(),
        serde_json::to_vec(&GenerationPointer::new(generation)).unwrap(),
    )
    .unwrap();
    install_active_runtime(&paths, b"[General]\r\nHintingMode=1\r\n");

    let error = ProtectedProfileInitializer::new(paths)
        .initialize()
        .err()
        .expect("mismatched runtime profile must fail initialization");

    assert_eq!(error.code, "runtime-profile-mismatch");
}

#[test]
fn initializer_refuses_ready_while_a_durable_activation_recovery_is_pending() {
    let (_base, paths) = paths();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    let mut catalog = ProfileCatalog::new();
    let generation = catalog
        .publish_machine_profile(
            bytes,
            SourceMetadata {
                display_name: "test".to_owned(),
            },
        )
        .unwrap();
    let directory = paths
        .profile_generations()
        .join(generation.directory_name());
    fs::create_dir_all(&directory).unwrap();
    fs::write(directory.join("profile.ini"), bytes).unwrap();
    fs::create_dir_all(paths.active_profile().parent().unwrap()).unwrap();
    fs::write(
        paths.active_profile(),
        serde_json::to_vec(&GenerationPointer::new(generation)).unwrap(),
    )
    .unwrap();
    install_active_runtime(&paths, bytes);
    fs::write(paths.profile_activation_journal(), b"pending").unwrap();

    let error = ProtectedProfileInitializer::new(paths.clone())
        .initialize()
        .err()
        .expect("pending activation recovery must prevent Ready");
    assert_eq!(error.code, "activation-recovery-required");

    fs::remove_file(paths.profile_activation_journal()).unwrap();
    fs::write(paths.runtime_activation_journal(), b"pending").unwrap();
    let error = ProtectedProfileInitializer::new(paths)
        .initialize()
        .err()
        .expect("pending runtime recovery must prevent Ready");
    assert_eq!(error.code, "activation-recovery-required");
}

#[test]
fn initializer_does_not_claim_ready_without_an_active_generation() {
    let (_base, paths) = paths();
    let error = ProtectedProfileInitializer::new(paths)
        .initialize()
        .err()
        .expect("missing active profile must fail initialization");
    assert_eq!(error.code, "active-profile-unavailable");
}
