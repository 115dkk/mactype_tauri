use mactype_service_contract::{
    validate_protected_renderer_profile, ProfileCatalog, ProtectedRendererProfileError,
    SourceMetadata,
};

fn metadata() -> SourceMetadata {
    SourceMetadata {
        display_name: "protected renderer contract".to_owned(),
    }
}

fn utf16_profile(text: &str, little_endian: bool) -> Vec<u8> {
    let mut bytes = if little_endian {
        vec![0xff, 0xfe]
    } else {
        vec![0xfe, 0xff]
    };
    for unit in text.encode_utf16() {
        let encoded = if little_endian {
            unit.to_le_bytes()
        } else {
            unit.to_be_bytes()
        };
        bytes.extend_from_slice(&encoded);
    }
    bytes
}

#[test]
fn protected_renderer_profile_requires_general_even_when_generic_ini_is_valid() {
    for profile in [
        b"[Other]\r\nHintingMode=0\r\n".as_slice(),
        b"[ General ]\r\nHintingMode=0\r\n".as_slice(),
    ] {
        let mut portable = ProfileCatalog::new();
        portable
            .publish_machine_profile(profile, metadata())
            .expect("portable profile validation deliberately remains general");

        assert_eq!(
            validate_protected_renderer_profile(profile),
            Err(ProtectedRendererProfileError::MissingGeneralSection)
        );
    }
}

#[test]
fn protected_renderer_profile_rejects_relative_and_absolute_alternative_files() {
    for alternative in [
        "profile.ini",
        r"ini\Default.ini",
        r"C:\ProgramData\MacType\profile.ini",
        r"\\server\share\profile.ini",
    ] {
        let profile = format!("[General]\r\nAlternativeFile={alternative}\r\n");
        let mut portable = ProfileCatalog::new();
        portable
            .publish_machine_profile(profile.as_bytes(), metadata())
            .expect("portable profile generations retain AlternativeFile compatibility");

        assert_eq!(
            validate_protected_renderer_profile(profile.as_bytes()),
            Err(ProtectedRendererProfileError::AlternativeFileNotAllowed),
            "protected profile accepted {alternative}"
        );
    }
}

#[test]
fn protected_renderer_profile_allows_missing_or_empty_alternative_file() {
    for profile in [
        b"[General]\r\nHintingMode=0\r\n".as_slice(),
        b"[General]\r\nAlternativeFile=\r\nHintingMode=0\r\n".as_slice(),
        b"[general]\r\nalternativefile=   \r\nHintingMode=0\r\n".as_slice(),
    ] {
        validate_protected_renderer_profile(profile).unwrap();
    }
}

#[test]
fn protected_renderer_profile_matches_existing_bom_and_utf16_support() {
    let plain = "[General]\r\nHintingMode=0\r\n";
    let mut utf8_bom = vec![0xef, 0xbb, 0xbf];
    utf8_bom.extend_from_slice(plain.as_bytes());

    for profile in [
        utf8_bom,
        utf16_profile(plain, true),
        utf16_profile(plain, false),
    ] {
        validate_protected_renderer_profile(&profile).unwrap();
    }
}

#[test]
fn utf16_alternative_file_is_rejected_by_the_protected_adapter_only() {
    let profile = utf16_profile("[General]\r\nAlternativeFile=profile.ini\r\n", true);
    let mut portable = ProfileCatalog::new();
    portable
        .publish_machine_profile(&profile, metadata())
        .expect("portable UTF-16 profile remains valid");

    assert_eq!(
        validate_protected_renderer_profile(&profile),
        Err(ProtectedRendererProfileError::AlternativeFileNotAllowed)
    );
}
