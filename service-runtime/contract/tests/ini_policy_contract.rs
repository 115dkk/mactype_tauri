use mactype_service_contract::{
    validate_protected_renderer_profile, ConsoleProcessPolicy, PrivateFreeTypePolicy,
    ProtectedRendererProfileError, UnityFontHookMode, UnityFontHookPolicy,
};

#[test]
fn shared_scan_contract_covers_comments_whitespace_and_duplicate_keys() {
    let profile = b"\
; comment\r\n\
# another comment\r\n\
[ General ]\r\n\
SkipConsoleProcesses = 0\r\n\
SkipConsoleProcesses = 1\r\n\
SkipPrivateFreeType = 1\r\n\
UnityFontHook = 1\r\n\
[UnityInclude]\r\n\
 C:\\Games\\First.exe \r\n\
 C:\\Games\\First.exe \r\n";

    assert!(ConsoleProcessPolicy::from_profile_bytes(profile).skip_console());
    assert!(PrivateFreeTypePolicy::from_profile_bytes(profile).skip_detected());
    let unity = UnityFontHookPolicy::from_profile_bytes(profile);
    assert_eq!(unity.mode(), UnityFontHookMode::SelectedGames);
    assert_eq!(unity.selected_games(), &["first.exe"]);
}

#[test]
fn shared_scan_contract_handles_missing_and_malformed_sections_with_bounded_defaults() {
    let profile = b"\
SkipConsoleProcesses=1\n\
[General\n\
SkipPrivateFreeType=1\n\
UnityFontHook=3\n\
[Other]\n\
AlternativeFile=outside.ini\n";

    assert!(!ConsoleProcessPolicy::from_profile_bytes(profile).skip_console());
    assert!(!PrivateFreeTypePolicy::from_profile_bytes(profile).skip_detected());
    assert_eq!(
        UnityFontHookPolicy::from_profile_bytes(profile).mode(),
        UnityFontHookMode::Off
    );
    assert!(matches!(
        validate_protected_renderer_profile(profile),
        Err(ProtectedRendererProfileError::InvalidProfile(_))
    ));
}
