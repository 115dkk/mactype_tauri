use mactype_service_contract::{UnityFontHookMode, UnityFontHookPolicy};

#[test]
fn unity_hook_defaults_off_and_rejects_out_of_range_modes() {
    for profile in [
        b"[General]\r\nHintingMode=0\r\n".as_slice(),
        b"[General]\r\nUnityFontHook=9\r\n".as_slice(),
        b"[General]\r\nUnityFontHook=invalid\r\n".as_slice(),
    ] {
        let policy = UnityFontHookPolicy::from_profile_bytes(profile);
        assert_eq!(policy.mode(), UnityFontHookMode::Off);
        assert!(!policy.applies_to("game.exe"));
    }
}

#[test]
fn selected_and_all_modes_use_case_insensitive_executable_lists() {
    let selected = UnityFontHookPolicy::from_profile_bytes(
        b"[General]\r\nUnityFontHook=1\r\n[UnityInclude]\r\nRebel Inc Escalation.exe\r\nC:\\Games\\PlagueIncEvolved.exe\r\n",
    );
    assert_eq!(selected.mode(), UnityFontHookMode::SelectedGames);
    assert!(selected.applies_to("rebel inc escalation.EXE"));
    assert!(selected.applies_to("PlagueIncEvolved.exe"));
    assert!(!selected.applies_to("other.exe"));

    let all = UnityFontHookPolicy::from_profile_bytes(
        b"[General]\r\nUnityFontHook=3\r\n[UnityExclude]\r\nfragile.exe\r\n",
    );
    assert_eq!(all.mode(), UnityFontHookMode::AllGames);
    assert!(!all.applies_to("FRAGILE.EXE"));
    assert!(all.applies_to("other.exe"));
}

#[test]
fn most_mode_applies_without_turning_unknown_values_into_all_mode() {
    let policy = UnityFontHookPolicy::from_profile_bytes(
        b"[General]\nUnityFontHook=2\n[UnityExclude]\nignored.exe\n",
    );
    assert_eq!(policy.mode(), UnityFontHookMode::MostGames);
    assert!(policy.applies_to("ignored.exe"));
}
