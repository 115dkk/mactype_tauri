//! Detection of the legacy MacTray tray mode.
//!
//! Tray mode is MacTray.exe running inside an interactive session and
//! injecting MacType on its own, which races the new service. Session 0 hosts
//! the legacy SCM service (`MacTray.exe -service`) and stays under the
//! migration flow, so it never counts as a tray-mode conflict.

pub(super) fn tray_mode_image(image_name: &str) -> bool {
    image_name.eq_ignore_ascii_case("MacTray.exe")
}

pub(super) fn tray_mode_session(session_id: Option<u32>) -> bool {
    // An unreadable session fails closed: only a verified session 0 process
    // is treated as the SCM-hosted legacy service.
    session_id != Some(0)
}

pub(super) fn legacy_tray_conflict() -> Result<bool, String> {
    #[cfg(windows)]
    {
        windows::interactive_mactray_running()
    }
    #[cfg(not(windows))]
    {
        Ok(false)
    }
}

pub(super) fn legacy_tray_conflict_detected() -> bool {
    legacy_tray_conflict().unwrap_or(true)
}

#[cfg(windows)]
mod windows {
    use windows_sys::Win32::{
        Foundation::GetLastError,
        System::RemoteDesktop::{WTSEnumerateProcessesW, WTSFreeMemory, WTS_PROCESS_INFOW},
    };

    struct ProcessList(*mut WTS_PROCESS_INFOW);

    impl Drop for ProcessList {
        fn drop(&mut self) {
            unsafe { WTSFreeMemory(self.0.cast()) };
        }
    }

    unsafe fn image_name(pointer: *const u16) -> String {
        if pointer.is_null() {
            return String::new();
        }
        let mut length = 0;
        while unsafe { *pointer.add(length) } != 0 {
            length += 1;
        }
        String::from_utf16_lossy(unsafe { std::slice::from_raw_parts(pointer, length) })
    }

    pub(super) fn interactive_mactray_running() -> Result<bool, String> {
        // WTS enumeration reports every process with its session id even for
        // an unelevated caller; per-process session queries fail with
        // ERROR_ACCESS_DENIED on the session-0 legacy service process.
        let mut processes = std::ptr::null_mut();
        let mut count = 0u32;
        if unsafe { WTSEnumerateProcessesW(std::ptr::null_mut(), 0, 1, &mut processes, &mut count) }
            == 0
        {
            return Err(format!(
                "the process enumeration failed with win32 error {}",
                unsafe { GetLastError() }
            ));
        }
        let list = ProcessList(processes);
        if count == 0 {
            return Ok(false);
        }
        let entries = unsafe { std::slice::from_raw_parts(list.0, count as usize) };
        Ok(entries.iter().any(|entry| {
            super::tray_mode_image(&unsafe { image_name(entry.pProcessName) })
                && super::tray_mode_session(Some(entry.SessionId))
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_mactray_image_is_a_tray_mode_candidate() {
        assert!(tray_mode_image("MacTray.exe"));
        assert!(tray_mode_image("mactray.EXE"));
        assert!(!tray_mode_image("MacTray"));
        assert!(!tray_mode_image("mactype-service.exe"));
        assert!(!tray_mode_image("MacType Control Center.exe"));
    }

    /// Driven by the machine-scoped demo workflow in
    /// `.github/workflows/live-tray-probe.yml`; it asserts the live probe
    /// against the phase the workflow has staged on the real machine.
    #[test]
    #[ignore = "live probe for the machine-scoped demo workflow"]
    fn live_probe_matches_expectation() {
        let expected = match std::env::var("MACTYPE_LIVE_TRAY_EXPECT").as_deref() {
            Ok("conflict") => true,
            Ok("clear") => false,
            other => panic!("MACTYPE_LIVE_TRAY_EXPECT must be conflict or clear, got {other:?}"),
        };
        assert_eq!(legacy_tray_conflict(), Ok(expected));
    }

    #[test]
    fn only_a_verified_service_session_escapes_the_tray_mode_conflict() {
        assert!(!tray_mode_session(Some(0)));
        assert!(tray_mode_session(Some(1)));
        assert!(tray_mode_session(Some(7)));
        assert!(tray_mode_session(None));
    }
}
