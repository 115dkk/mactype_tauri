use mactype_service_contract::PrivateFreeTypePolicy;

#[test]
fn private_freetype_skip_is_opt_in() {
    assert!(
        !PrivateFreeTypePolicy::from_profile_bytes(b"[General]\r\nHintingMode=1\r\n")
            .skip_detected()
    );
    assert!(
        PrivateFreeTypePolicy::from_profile_bytes(b"[General]\r\nSkipPrivateFreeType=1\r\n",)
            .skip_detected()
    );
}
