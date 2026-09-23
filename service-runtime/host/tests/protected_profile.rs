#[path = "support/protected_tree.rs"]
mod protected_tree_support;

use protected_tree_support::ProtectedTree;
use std::fs;

use mactype_service_contract::{GenerationId, GenerationPointer, ProfileCatalog, SourceMetadata};
use mactype_service_host::{
    ProtectedRendererRuntime, ACTIVE_PROFILE_ABSENT_CODE, RUNTIME_PROFILE_ABSENT_CODE,
};

#[test]
fn initializer_rejects_an_oversized_active_profile_pointer_before_parsing() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    fs::create_dir_all(paths.active_profile().parent().unwrap()).unwrap();
    fs::write(paths.active_profile(), vec![b'x'; 64 * 1024 + 1]).unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("oversized active profile pointer must fail initialization");

    assert_eq!(error.code, "active-profile-invalid");
    assert!(error.message.contains("bounded regular file"));
}

#[test]
fn initializer_rejects_an_oversized_runtime_pointer_before_parsing() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(bytes);
    fs::create_dir_all(paths.runtime_pointer().parent().unwrap()).unwrap();
    fs::write(paths.runtime_pointer(), vec![b'x'; 64 * 1024 + 1]).unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("oversized runtime pointer must fail initialization");

    assert_eq!(error.code, "active-runtime-invalid");
    assert!(error.message.contains("bounded regular file"));
}

#[test]
fn initializer_reports_the_verified_protected_active_profile_digest() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
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
    tree.install_runtime(Some(bytes));

    let runtime = ProtectedRendererRuntime::load(paths.clone()).unwrap();
    assert_eq!(
        runtime.binding().profile_digest().as_str(),
        generation.as_str()
    );
    assert_eq!(
        runtime.binding().runtime_generation_id(),
        runtime.assets().generation_id()
    );

    fs::write(
        directory.join("profile.ini"),
        b"[General]\r\nHintingMode=1\r\n",
    )
    .unwrap();
    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("tampered profile must fail initialization");
    assert_eq!(error.code, "active-profile-tampered");
}

#[test]
fn initializer_carries_the_verified_private_freetype_policy() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nSkipPrivateFreeType=1\r\n";
    tree.install_active_profile(bytes);
    tree.install_runtime(Some(bytes));

    let runtime = ProtectedRendererRuntime::load(paths).unwrap();

    assert!(runtime.private_freetype_policy().skip_detected());
}

#[test]
fn initializer_carries_the_verified_console_process_policy() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nSkipConsoleProcesses=1\r\n";
    tree.install_active_profile(bytes);
    tree.install_runtime(Some(bytes));

    let runtime = ProtectedRendererRuntime::load(paths).unwrap();

    assert!(runtime.console_process_policy().skip_console());
}

#[test]
fn initializer_reports_an_absent_generated_runtime_profile() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(bytes);
    let runtime = tree.install_runtime(Some(bytes));
    fs::remove_file(runtime.join("MacType.ini")).unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("the supported stopped runtime has no generated profile");

    assert_eq!(error.code, RUNTIME_PROFILE_ABSENT_CODE);
}

#[test]
fn initializer_rejects_a_dll_adjacent_profile_that_differs_from_the_active_generation() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
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
    tree.install_runtime(Some(
        b"[General]
HintingMode=1
",
    ));

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("mismatched runtime profile must fail initialization");

    assert_eq!(error.code, "runtime-profile-mismatch");
}

#[test]
fn initializer_refuses_ready_while_a_durable_activation_recovery_is_pending() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
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
    tree.install_runtime(Some(bytes));
    fs::write(paths.profile_activation_journal(), b"pending").unwrap();

    let error = ProtectedRendererRuntime::load(paths.clone())
        .expect_err("pending activation recovery must prevent Ready");
    assert_eq!(error.code, "activation-recovery-required");

    fs::remove_file(paths.profile_activation_journal()).unwrap();
    fs::write(paths.runtime_activation_journal(), b"pending").unwrap();
    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("pending runtime recovery must prevent Ready");
    assert_eq!(error.code, "activation-recovery-required");
}

#[test]
fn initializer_refuses_a_runtime_activation_receipt_for_a_different_candidate() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(bytes);
    tree.install_runtime(Some(bytes));
    fs::write(
        paths.runtime_activation_journal(),
        br#"{"schema":3,"phase":"committed","previous":null,"activated":{"schema":1,"version":"0.3.0"}}"#,
    )
    .unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("a receipt for a different runtime candidate must prevent Ready");

    assert_eq!(error.code, "activation-recovery-required");
}

#[test]
fn initializer_refuses_a_stale_matching_receipt_without_a_durable_commit_phase() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(bytes);
    tree.install_runtime(Some(bytes));
    fs::write(
        paths.runtime_activation_journal(),
        br#"{"schema":2,"previous":null,"activated":{"schema":1,"version":"0.2.0"}}"#,
    )
    .unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("a stale matching receipt without an explicit commit must prevent Ready");

    assert_eq!(error.code, "activation-recovery-required");
}

#[test]
fn initializer_refuses_legacy_uncommitted_and_rollback_required_receipts() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(bytes);
    tree.install_runtime(Some(bytes));

    for receipt in [
        br#"{"schema":1,"previous":null}"#.as_slice(),
        br#"{"schema":3,"phase":"candidate","previous":null,"activated":{"schema":1,"version":"0.2.0"}}"#
            .as_slice(),
        br#"{"schema":3,"phase":"rollback-required","previous":null,"activated":{"schema":1,"version":"0.2.0"}}"#
            .as_slice(),
    ] {
        fs::write(paths.runtime_activation_journal(), receipt).unwrap();
        let error = ProtectedRendererRuntime::load(paths.clone())
        .expect_err("legacy and uncommitted activation receipts must prevent Ready");
        assert_eq!(error.code, "activation-recovery-required");
    }
}

#[test]
fn initializer_does_not_claim_ready_without_an_active_generation() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("nothing has been published, so the start must end as a supported stop");

    assert_eq!(error.code, ACTIVE_PROFILE_ABSENT_CODE);
}

#[test]
fn initializer_reports_an_absent_active_profile_pointer_without_changing_runtime_selection() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let runtime_pointer = br#"{"schema":1,"version":"0.2.0"}"#;
    tree.install_runtime(Some(
        b"[General]
HintingMode=0
",
    ));

    let error = ProtectedRendererRuntime::load(paths.clone())
        .expect_err("missing active profile pointer must keep the service stopped");

    assert_eq!(error.code, ACTIVE_PROFILE_ABSENT_CODE);
    assert_eq!(
        error.message,
        "no profile has been published yet, so the service stays stopped until setup publishes one"
    );
    assert_eq!(fs::read(paths.runtime_pointer()).unwrap(), runtime_pointer);
}

#[test]
fn initializer_keeps_a_dangling_active_profile_pointer_unavailable() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(bytes);
    tree.install_runtime(Some(bytes));
    fs::remove_file(
        paths
            .profile_generations()
            .join(GenerationId::from_profile_bytes(bytes).directory_name())
            .join("profile.ini"),
    )
    .unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("a dangling profile generation must require repair");

    assert_eq!(error.code, "active-profile-unavailable");
}

#[test]
fn profile_activation_journal_takes_priority_over_an_absent_active_pointer() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    fs::create_dir_all(paths.profile_activation_journal().parent().unwrap()).unwrap();
    fs::write(paths.profile_activation_journal(), b"pending").unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("pending profile activation recovery must take priority");

    assert_eq!(error.code, "activation-recovery-required");
}

#[test]
fn protected_renderer_runtime_rejects_an_indirect_alternative_profile() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let bytes = b"[General]\r\nAlternativeFile=profile.ini\r\n";
    tree.install_active_profile(bytes);
    tree.install_runtime(Some(bytes));

    let error = ProtectedRendererRuntime::load(paths).unwrap_err();

    assert_eq!(error.code, "active-profile-invalid");
}

#[cfg(feature = "ci-test-adapter")]
#[test]
fn protected_renderer_runtime_rejects_a_profile_pointer_change_during_pairing() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let original = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(original);
    tree.install_runtime(Some(original));
    let replacement = b"[General]\r\nHintingMode=1\r\n";
    let replacement_generation =
        mactype_service_contract::GenerationId::from_profile_bytes(replacement);
    let replacement_root = paths
        .profile_generations()
        .join(replacement_generation.directory_name());
    fs::create_dir_all(&replacement_root).unwrap();
    fs::write(replacement_root.join("profile.ini"), replacement).unwrap();
    let replacement_pointer =
        serde_json::to_vec(&GenerationPointer::new(replacement_generation)).unwrap();

    let error =
        ProtectedRendererRuntime::load_with_pointer_stability_hook_for_ci(paths.clone(), || {
            fs::write(paths.active_profile(), replacement_pointer).unwrap()
        })
        .unwrap_err();

    assert_eq!(error.code, "renderer-runtime-binding-changed");
}

#[cfg(feature = "ci-test-adapter")]
#[test]
fn protected_renderer_runtime_rejects_a_runtime_pointer_change_during_pairing() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let profile = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(profile);
    tree.install_runtime(Some(profile));

    let error =
        ProtectedRendererRuntime::load_with_pointer_stability_hook_for_ci(paths.clone(), || {
            fs::write(
                paths.runtime_pointer(),
                br#"{"schema":1,"version":"0.3.0"}"#,
            )
            .unwrap()
        })
        .unwrap_err();

    assert_eq!(error.code, "renderer-runtime-binding-changed");
}

#[cfg(feature = "ci-test-adapter")]
#[test]
fn protected_renderer_runtime_rejects_runtime_content_changed_during_pairing() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let profile = b"[General]\r\nHintingMode=0\r\n";
    tree.install_active_profile(profile);
    let runtime_root = tree.install_runtime(Some(profile));

    let error = ProtectedRendererRuntime::load_with_pointer_stability_hook_for_ci(paths, || {
        fs::write(runtime_root.join("MacType.dll"), b"changed-core").unwrap()
    })
    .unwrap_err();

    assert_eq!(error.code, "renderer-runtime-content-changed");
}
