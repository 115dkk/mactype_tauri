use mactype_service_contract::ConsoleProcessPolicy;

#[test]
fn console_process_skip_is_opt_in() {
    assert!(
        !ConsoleProcessPolicy::from_profile_bytes(b"[General]\r\nHintingMode=1\r\n").skip_console()
    );
    assert!(
        ConsoleProcessPolicy::from_profile_bytes(b"[General]\r\nSkipConsoleProcesses=1\r\n")
            .skip_console()
    );
}

#[test]
fn console_process_policy_uses_only_the_general_section_and_exact_enabled_value() {
    assert!(!ConsoleProcessPolicy::from_profile_bytes(
        b"[Other]\r\nSkipConsoleProcesses=1\r\n[General]\r\nSkipConsoleProcesses=true\r\n",
    )
    .skip_console());
    assert!(ConsoleProcessPolicy::from_profile_bytes(
        b"; comment\r\n[ gEnErAl ]\r\n# another comment\r\nSkipConsoleProcesses=0\r\nskipconsoleprocesses = 1\r\n",
    )
    .skip_console());
    assert!(!ConsoleProcessPolicy::from_profile_bytes(
        b"[General]\r\nSkipConsoleProcesses=1\r\nSkipConsoleProcesses=0\r\n",
    )
    .skip_console());
}

#[test]
fn console_process_policy_accepts_utf16_profiles() {
    let text = "[General]\r\nSkipConsoleProcesses=1\r\n";
    let mut bytes = vec![0xff, 0xfe];
    bytes.extend(text.encode_utf16().flat_map(u16::to_le_bytes));

    assert!(ConsoleProcessPolicy::from_profile_bytes(&bytes).skip_console());
}
