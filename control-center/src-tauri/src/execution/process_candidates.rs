use serde::Serialize;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ManualLaunchCandidate {
    pub pid: u32,
    pub name: String,
    pub path: String,
    pub window_title: Option<String>,
}

pub(super) fn list_manual_launch_candidates_impl() -> Result<Vec<ManualLaunchCandidate>, String> {
    #[cfg(windows)]
    {
        windows::list_candidates()
    }
    #[cfg(not(windows))]
    {
        Ok(Vec::new())
    }
}

fn order_candidates(mut candidates: Vec<ManualLaunchCandidate>) -> Vec<ManualLaunchCandidate> {
    let mut seen = std::collections::HashSet::new();
    candidates.retain(|candidate| seen.insert(candidate.pid));
    candidates.sort_by(|left, right| {
        right
            .window_title
            .is_some()
            .cmp(&left.window_title.is_some())
            .then_with(|| left.name.to_lowercase().cmp(&right.name.to_lowercase()))
            .then_with(|| left.pid.cmp(&right.pid))
    });
    candidates
}

#[cfg_attr(not(windows), allow(dead_code))]
fn is_under_directory(path: &str, root: &str) -> bool {
    let normalized_path = path.replace('/', "\\").to_lowercase();
    let normalized_root = root.replace('/', "\\").to_lowercase();
    let normalized_root = normalized_root.trim_end_matches('\\');
    if normalized_root.is_empty() {
        return false;
    }
    normalized_path == normalized_root
        || normalized_path.starts_with(&format!("{normalized_root}\\"))
}

#[cfg(windows)]
mod windows {
    use super::{is_under_directory, order_candidates, ManualLaunchCandidate};
    use std::{collections::HashMap, env, ffi::OsString, os::windows::ffi::OsStringExt};
    use windows_sys::Win32::{
        Foundation::{CloseHandle, HANDLE, HWND, LPARAM},
        System::{
            RemoteDesktop::{
                ProcessIdToSessionId, WTSEnumerateProcessesW, WTSFreeMemory, WTS_PROCESS_INFOW,
            },
            Threading::{
                GetCurrentProcessId, OpenProcess, QueryFullProcessImageNameW, PROCESS_NAME_WIN32,
                PROCESS_QUERY_LIMITED_INFORMATION,
            },
        },
        UI::WindowsAndMessaging::{
            EnumWindows, GetWindowLongW, GetWindowTextW, GetWindowThreadProcessId, IsWindowVisible,
            GWL_EXSTYLE, WS_EX_TOOLWINDOW,
        },
    };

    pub(super) fn list_candidates() -> Result<Vec<ManualLaunchCandidate>, String> {
        let current_pid = unsafe { GetCurrentProcessId() };
        let mut current_session = 0_u32;
        if unsafe { ProcessIdToSessionId(current_pid, &mut current_session) } == 0 {
            return Err("the current session could not be identified".to_owned());
        }
        let window_titles = visible_window_titles();
        let windows_directory = env::var_os("WINDIR")
            .or_else(|| env::var_os("SystemRoot"))
            .map(|value| value.to_string_lossy().into_owned());
        let mut candidates = Vec::new();
        for process in enumerate_processes()? {
            if process.pid == 0 || process.pid == 4 || process.pid == current_pid {
                continue;
            }
            if process.session_id != current_session {
                continue;
            }
            let Some(path) = process_image_path(process.pid) else {
                continue;
            };
            if windows_directory
                .as_deref()
                .is_some_and(|root| is_under_directory(&path, root))
            {
                continue;
            }
            let name = path
                .rsplit(['\\', '/'])
                .next()
                .unwrap_or(path.as_str())
                .to_owned();
            candidates.push(ManualLaunchCandidate {
                pid: process.pid,
                name,
                path,
                window_title: window_titles.get(&process.pid).cloned(),
            });
        }
        Ok(order_candidates(candidates))
    }

    /// Maps each owning PID to the title of its topmost visible top-level
    /// window; tool windows and untitled windows never contribute a title.
    fn visible_window_titles() -> HashMap<u32, String> {
        struct Context {
            titles: HashMap<u32, String>,
        }

        unsafe extern "system" fn collect(window: HWND, parameter: LPARAM) -> i32 {
            let context = unsafe { &mut *(parameter as *mut Context) };
            if unsafe { IsWindowVisible(window) } == 0 {
                return 1;
            }
            if unsafe { GetWindowLongW(window, GWL_EXSTYLE) } as u32 & WS_EX_TOOLWINDOW != 0 {
                return 1;
            }
            let mut owner_pid = 0_u32;
            unsafe { GetWindowThreadProcessId(window, &mut owner_pid) };
            if owner_pid == 0 || context.titles.contains_key(&owner_pid) {
                return 1;
            }
            let mut buffer = [0_u16; 512];
            let length =
                unsafe { GetWindowTextW(window, buffer.as_mut_ptr(), buffer.len() as i32) };
            if length <= 0 {
                return 1;
            }
            let title = String::from_utf16_lossy(&buffer[..length as usize]);
            if title.trim().is_empty() {
                return 1;
            }
            context.titles.insert(owner_pid, title);
            1
        }

        let mut context = Context {
            titles: HashMap::new(),
        };
        unsafe { EnumWindows(Some(collect), (&mut context as *mut Context) as LPARAM) };
        context.titles
    }

    struct EnumeratedProcess {
        pid: u32,
        session_id: u32,
    }

    struct WtsProcessList(*mut WTS_PROCESS_INFOW);

    impl Drop for WtsProcessList {
        fn drop(&mut self) {
            if !self.0.is_null() {
                unsafe { WTSFreeMemory(self.0.cast()) };
            }
        }
    }

    fn enumerate_processes() -> Result<Vec<EnumeratedProcess>, String> {
        let mut processes = std::ptr::null_mut();
        let mut count = 0_u32;
        if unsafe { WTSEnumerateProcessesW(std::ptr::null_mut(), 0, 1, &mut processes, &mut count) }
            == 0
        {
            return Err("the running process inventory could not be enumerated".to_owned());
        }
        let list = WtsProcessList(processes);
        if count == 0 || list.0.is_null() {
            return Ok(Vec::new());
        }
        let entries = unsafe { std::slice::from_raw_parts(list.0, count as usize) };
        Ok(entries
            .iter()
            .map(|entry| EnumeratedProcess {
                pid: entry.ProcessId,
                session_id: entry.SessionId,
            })
            .collect())
    }

    struct ProcessHandle(HANDLE);

    impl Drop for ProcessHandle {
        fn drop(&mut self) {
            unsafe { CloseHandle(self.0) };
        }
    }

    fn process_image_path(pid: u32) -> Option<String> {
        let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
        if handle.is_null() {
            return None;
        }
        let handle = ProcessHandle(handle);
        let mut buffer = vec![0_u16; 32_768];
        let mut length = buffer.len() as u32;
        if unsafe {
            QueryFullProcessImageNameW(
                handle.0,
                PROCESS_NAME_WIN32,
                buffer.as_mut_ptr(),
                &mut length,
            )
        } == 0
        {
            return None;
        }
        if length == 0 || length as usize >= buffer.len() {
            return None;
        }
        Some(
            OsString::from_wide(&buffer[..length as usize])
                .to_string_lossy()
                .into_owned(),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn candidate(pid: u32, name: &str, window_title: Option<&str>) -> ManualLaunchCandidate {
        ManualLaunchCandidate {
            pid,
            name: name.to_owned(),
            path: format!("C:\\Tools\\{name}"),
            window_title: window_title.map(str::to_owned),
        }
    }

    #[test]
    fn candidates_list_windowed_processes_first_in_alphabetical_groups() {
        let ordered = order_candidates(vec![
            candidate(10, "zulu.exe", None),
            candidate(20, "notepad.exe", Some("Untitled - Notepad")),
            candidate(30, "alpha.exe", None),
            candidate(40, "Code.exe", Some("Visual Studio Code")),
        ]);

        let names: Vec<&str> = ordered.iter().map(|entry| entry.name.as_str()).collect();
        assert_eq!(names, ["Code.exe", "notepad.exe", "alpha.exe", "zulu.exe"]);
    }

    #[test]
    fn candidates_are_deduplicated_by_pid() {
        let ordered = order_candidates(vec![
            candidate(20, "notepad.exe", Some("Untitled - Notepad")),
            candidate(20, "notepad.exe", None),
        ]);

        assert_eq!(ordered.len(), 1);
        assert_eq!(
            ordered[0].window_title.as_deref(),
            Some("Untitled - Notepad")
        );
    }

    #[test]
    fn windows_directory_exclusion_is_case_insensitive_and_boundary_safe() {
        assert!(is_under_directory(
            "C:\\Windows\\System32\\notepad.exe",
            "C:\\WINDOWS"
        ));
        assert!(is_under_directory(
            "C:/windows/explorer.exe",
            "C:\\Windows\\"
        ));
        assert!(!is_under_directory(
            "C:\\WindowsApps\\tool.exe",
            "C:\\Windows"
        ));
        assert!(!is_under_directory("C:\\Tools\\notepad.exe", "C:\\Windows"));
        assert!(!is_under_directory("C:\\Tools\\notepad.exe", ""));
    }
}
