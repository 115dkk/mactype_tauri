#![cfg(all(windows, feature = "ci-test-adapter"))]

use std::fs;

use mactype_service_setup::{
    service_configuration_matches_owned_contract, service_image_matches_protected_contract,
    ObservedServiceConfiguration,
};

#[test]
fn only_the_fixed_service_binary_below_the_protected_runtime_is_owned() {
    let base = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
    let root = base
        .path()
        .join("Program Files")
        .join("MacType Control Center")
        .join("Service");
    let binary = root.join("bin").join("0.2.0").join("mactype-service.exe");
    fs::create_dir_all(binary.parent().unwrap()).unwrap();
    fs::write(&binary, b"service").unwrap();

    assert!(service_image_matches_protected_contract(
        &root,
        &format!(r#""{}" --service"#, binary.display())
    ));
    assert!(!service_image_matches_protected_contract(
        &root,
        &format!(r#""{}" --service OtherName"#, binary.display())
    ));
    assert!(!service_image_matches_protected_contract(
        &root,
        &format!(r#""{}"  --service"#, binary.display())
    ));
    assert!(!service_image_matches_protected_contract(
        &root,
        &format!(r#""{}" --service "#, binary.display())
    ));
    assert!(!service_image_matches_protected_contract(
        &root,
        &format!(r#"{} --service"#, binary.display())
    ));
    assert!(!service_image_matches_protected_contract(
        &root,
        r#""C:\Program Files\MacType\MacTray.exe" -service"#
    ));
    assert!(!service_image_matches_protected_contract(
        &root,
        r#""C:\Users\person\AppData\Local\mactype-service.exe" --service"#
    ));
}

#[test]
fn foreign_collisions_never_match_the_full_core_service_identity() {
    let base = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
    let root = base
        .path()
        .join("Program Files")
        .join("MacType Control Center")
        .join("Service");
    let binary = root.join("bin").join("0.2.0").join("mactype-service.exe");
    fs::create_dir_all(binary.parent().unwrap()).unwrap();
    fs::write(&binary, b"service").unwrap();
    let image = format!(r#""{}" --service"#, binary.display());
    let owned = |service_type,
                 start_type,
                 error_control,
                 account: &str,
                 display: &str,
                 group: &str,
                 tag,
                 dependencies: &[String]| {
        service_configuration_matches_owned_contract(
            &root,
            &ObservedServiceConfiguration {
                service_type,
                start_type,
                error_control,
                image_path: &image,
                account,
                display_name: display,
                load_order_group: group,
                tag_id: tag,
                dependencies,
            },
        )
    };
    assert!(owned(
        0x10,
        2,
        1,
        "LocalSystem",
        "MacType Control Center Service",
        "",
        0,
        &[]
    ));
    assert!(!owned(
        0x10,
        2,
        0,
        "LocalSystem",
        "MacType Control Center Service",
        "",
        0,
        &[]
    ));
    assert!(!owned(
        0x10,
        2,
        1,
        "LocalSystem",
        "Foreign Display",
        "",
        0,
        &[]
    ));
    assert!(!owned(
        0x10,
        2,
        1,
        "LocalSystem",
        "MacType Control Center Service",
        "group",
        1,
        &[]
    ));
    assert!(!owned(
        0x10,
        2,
        1,
        "LocalSystem",
        "MacType Control Center Service",
        "",
        0,
        &["winmgmt".to_owned()],
    ));
}
