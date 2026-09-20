#[path = "support/protected_tree.rs"]
mod protected_tree_support;

use protected_tree_support::ProtectedTree;

fn install_runtime_fixture(tree: &ProtectedTree) -> std::path::PathBuf {
    let profile = b"[General]
HintingMode=0
";
    tree.install_active_profile(profile);
    tree.install_runtime(Some(profile))
}
use std::fs;

use mactype_service_contract::{
    IMMUTABLE_RUNTIME_FILES, MAX_PROFILE_BYTES, MAX_RUNTIME_FILE_BYTES,
};
use mactype_service_host::{ProtectedRendererRuntime, RUNTIME_PROFILE_ABSENT_CODE};

#[test]
fn active_runtime_rejects_each_oversized_immutable_component_at_the_file_boundary() {
    for oversized_name in IMMUTABLE_RUNTIME_FILES {
        let tree = ProtectedTree::new();
        let paths = tree.paths().clone();
        let generation = install_runtime_fixture(&tree);
        fs::File::create(generation.join(oversized_name))
            .unwrap()
            .set_len(MAX_RUNTIME_FILE_BYTES as u64 + 1)
            .unwrap();

        let error = ProtectedRendererRuntime::load(paths).unwrap_err();

        assert_eq!(error.code, "runtime-component-invalid", "{oversized_name}");
        assert!(error.message.contains("bounded"), "{oversized_name}");
    }
}

#[test]
fn active_runtime_rejects_an_oversized_generated_profile_at_the_profile_boundary() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let generation = install_runtime_fixture(&tree);
    fs::File::create(generation.join("MacType.ini"))
        .unwrap()
        .set_len(MAX_PROFILE_BYTES as u64 + 1)
        .unwrap();

    let error = ProtectedRendererRuntime::load(paths).unwrap_err();

    assert_eq!(error.code, "runtime-component-invalid");
    assert!(error.message.contains("bounded"));
}

#[test]
fn helpers_and_dlls_are_selected_only_from_the_active_protected_runtime_generation() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let generation = install_runtime_fixture(&tree);

    let runtime = ProtectedRendererRuntime::load(paths.clone()).unwrap();
    let assets = runtime.assets();

    assert_eq!(assets.root(), generation);
    assert_eq!(
        assets.injector32(),
        generation.join("mactype-injector32.exe")
    );
    assert_eq!(
        assets.injector64(),
        generation.join("mactype-injector64.exe")
    );
    assert_eq!(assets.generation_id().as_str().len(), 64);
    assert!(assets
        .generation_id()
        .as_str()
        .bytes()
        .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase()));
}

#[test]
fn active_runtime_reports_a_missing_generated_profile_as_a_supported_stop() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let generation = install_runtime_fixture(&tree);
    fs::remove_file(generation.join("MacType.ini")).unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("a stopped runtime has no generated profile");

    assert_eq!(error.code, RUNTIME_PROFILE_ABSENT_CODE);
}

#[test]
fn active_runtime_with_a_missing_profile_and_stray_file_remains_invalid() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let generation = install_runtime_fixture(&tree);
    fs::remove_file(generation.join("MacType.ini")).unwrap();
    fs::write(generation.join("unsigned.dll"), b"unexpected").unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("a stray runtime file must not become a supported stop");

    assert_eq!(error.code, "runtime-file-set-invalid");
}

#[test]
fn active_runtime_rejects_every_file_beyond_manifest_assets_and_generated_mactype_ini() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    let generation = install_runtime_fixture(&tree);
    fs::write(generation.join("unsigned.dll"), b"unexpected").unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("an unexpected runtime file must fail initialization");

    assert_eq!(error.code, "runtime-file-set-invalid");
}

#[test]
fn active_runtime_rejects_an_oversized_pointer_before_parsing() {
    let tree = ProtectedTree::new();
    let paths = tree.paths().clone();
    install_runtime_fixture(&tree);
    fs::write(paths.runtime_pointer(), vec![b'x'; 64 * 1024 + 1]).unwrap();

    let error = ProtectedRendererRuntime::load(paths)
        .expect_err("an oversized runtime pointer must fail before JSON parsing");

    assert_eq!(error.code, "active-runtime-invalid");
    assert!(error.message.contains("bounded regular file"));
}
