mod operation_log;

use crate::preview::{PreviewDiagnosticSnapshot, PreviewState};
use operation_log::read_recent_operation_logs;
pub(crate) use operation_log::{record_operation_failure, OperationFailure};
use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};
use tauri::State;

pub fn log_root() -> Result<PathBuf, String> {
    env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .map(|path| path.join("MacType").join("ControlCenter").join("logs"))
        .ok_or_else(|| "LOCALAPPDATA is not available".to_owned())
}

fn export_to(directory: &Path, report: &str, timestamp: u128) -> Result<PathBuf, String> {
    fs::create_dir_all(directory).map_err(|error| error.to_string())?;
    let destination = directory.join(format!("diagnostics-{timestamp}.txt"));
    let temporary = directory.join(format!(".diagnostics-{timestamp}.tmp"));
    fs::write(&temporary, report.as_bytes()).map_err(|error| error.to_string())?;
    fs::rename(&temporary, &destination).map_err(|error| {
        let _ = fs::remove_file(&temporary);
        error.to_string()
    })?;
    Ok(destination)
}

pub fn export(report: &str) -> Result<String, String> {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| error.to_string())?
        .as_nanos();
    export_to(&log_root()?, report, timestamp).map(|path| path.to_string_lossy().into_owned())
}

#[tauri::command]
pub(crate) fn open_log_folder() -> Result<String, String> {
    let directory = log_root()?;
    fs::create_dir_all(&directory).map_err(|error| error.to_string())?;
    #[cfg(windows)]
    Command::new("explorer.exe")
        .arg(&directory)
        .spawn()
        .map_err(|error| error.to_string())?;
    #[cfg(not(windows))]
    return Err("opening the log folder is supported only on Windows".to_owned());
    Ok(directory.to_string_lossy().into_owned())
}

#[cfg(windows)]
pub fn copy_to_clipboard(report: &str) -> Result<(), String> {
    use windows_sys::Win32::{
        Foundation::{GlobalFree, HANDLE},
        System::{
            DataExchange::{CloseClipboard, EmptyClipboard, OpenClipboard, SetClipboardData},
            Memory::{GlobalAlloc, GlobalLock, GlobalUnlock, GMEM_MOVEABLE},
            Ole::CF_UNICODETEXT,
        },
    };

    let wide = report.encode_utf16().chain(Some(0)).collect::<Vec<_>>();
    unsafe {
        if OpenClipboard(std::ptr::null_mut()) == 0 {
            return Err("could not open the Windows clipboard".to_owned());
        }
        if EmptyClipboard() == 0 {
            CloseClipboard();
            return Err("could not clear the Windows clipboard".to_owned());
        }
        let handle = GlobalAlloc(GMEM_MOVEABLE, wide.len() * size_of::<u16>());
        if handle.is_null() {
            CloseClipboard();
            return Err("could not allocate clipboard memory".to_owned());
        }
        let target = GlobalLock(handle).cast::<u16>();
        if target.is_null() {
            GlobalFree(handle);
            CloseClipboard();
            return Err("could not lock clipboard memory".to_owned());
        }
        std::ptr::copy_nonoverlapping(wide.as_ptr(), target, wide.len());
        GlobalUnlock(handle);
        if SetClipboardData(CF_UNICODETEXT as u32, handle as HANDLE).is_null() {
            GlobalFree(handle);
            CloseClipboard();
            return Err("could not publish clipboard data".to_owned());
        }
        CloseClipboard();
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn copy_to_clipboard(_report: &str) -> Result<(), String> {
    Err("copying diagnostics is supported only on Windows".to_owned())
}

fn diagnostic_report_text(snapshot: PreviewDiagnosticSnapshot) -> String {
    let status = snapshot.status;
    let mut report = String::from("MacType Control Center diagnostics\n");
    report.push_str(&format!(
        "controlCenterVersion={}\n",
        env!("CARGO_PKG_VERSION")
    ));
    report.push_str(&format!("os={}\n", env::consts::OS));
    report.push_str(&format!("arch={}\n", env::consts::ARCH));
    report.push_str(&format!("state={}\n", status.state));
    report.push_str(&format!(
        "installationRoot={}\n",
        status.root.as_deref().unwrap_or("not-found")
    ));
    report.push_str(&format!(
        "coreVersion={}\n",
        status.core_version.as_deref().unwrap_or("unknown")
    ));
    for finding in status.findings {
        report.push_str(&format!(
            "finding.{}={} ({})\n",
            finding.label,
            finding.value,
            if finding.ok { "ok" } else { "failed" }
        ));
    }
    let entries = snapshot.entries;
    report.push_str(&format!("previewLogEntries={}\n", entries.len()));
    for entry in entries.iter().rev().take(20).rev() {
        report.push_str("previewLog=");
        report.push_str(&entry.replace(['\r', '\n'], " "));
        report.push('\n');
    }
    report
}

#[tauri::command]
pub(crate) fn diagnostic_report(state: State<'_, PreviewState>) -> Result<String, String> {
    let mut report = diagnostic_report_text(state.diagnostic_snapshot()?);
    append_operation_logs(&mut report, &read_recent_operation_logs(50)?);
    Ok(report)
}

fn append_operation_logs(report: &mut String, entries: &[operation_log::OperationLogEntry]) {
    report.push_str(&format!("operationLogEntries={}\n", entries.len()));
    for entry in entries {
        report.push_str("operationLog=");
        report.push_str(&entry.render());
        report.push('\n');
    }
}

#[tauri::command]
pub(crate) fn diagnostic_recent_logs() -> Result<Vec<String>, String> {
    read_recent_operation_logs(50).map(|entries| {
        entries
            .iter()
            .map(operation_log::OperationLogEntry::render)
            .collect()
    })
}

#[tauri::command]
pub(crate) fn export_diagnostics(state: State<'_, PreviewState>) -> Result<String, String> {
    let report = diagnostic_report(state)?;
    export(&report)
}

#[tauri::command]
pub(crate) fn copy_diagnostics(state: State<'_, PreviewState>) -> Result<(), String> {
    let report = diagnostic_report(state)?;
    copy_to_clipboard(&report)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn diagnostic_export_is_atomic_and_preserves_unicode() {
        let root = env::temp_dir().join(format!("mactype-diagnostics-{}", std::process::id()));
        let path = export_to(&root, "MacType diagnostics\n코어=2022.7.12\n", 7).unwrap();
        assert_eq!(path.file_name().unwrap(), "diagnostics-7.txt");
        assert_eq!(
            fs::read_to_string(&path).unwrap(),
            "MacType diagnostics\n코어=2022.7.12\n"
        );
        assert!(!root.join(".diagnostics-7.tmp").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn operation_failures_survive_restart_without_profile_or_nonce_leaks() {
        let root = env::temp_dir().join(format!(
            "mactype-operation-log-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let profile = "[General]\r\nSecretFont=private\r\n";
        let nonce = "00112233445566778899aabbccddeeff";
        let long_error = format!(
            "setup broker start failed with Win32 5 (Access is denied): {profile}; token={nonce}; {}",
            "x".repeat(40 * 1024)
        );

        operation_log::record_operation_failure_at(
            &root,
            &OperationFailure {
                operation: "migrate-from-legacy".to_owned(),
                stage: "activate open service".to_owned(),
                error_chain: long_error,
                broker_exit_code: Some(21),
                channel_failure: None,
                rollback: "completed".to_owned(),
                final_state: "legacy=running/auto; modern=absent".to_owned(),
            },
            &[profile],
        )
        .unwrap();

        let entries = operation_log::read_recent_operation_logs_at(&root, 20).unwrap();
        assert_eq!(entries.len(), 1);
        let entry = &entries[0];
        assert_eq!(entry.operation, "migrate-from-legacy");
        assert_eq!(entry.win32_code, Some(5));
        assert!(entry.error_chain.contains("[truncated]"));
        let disk = fs::read_to_string(root.join("control-center.log")).unwrap();
        assert!(!disk.contains(profile), "{disk}");
        assert!(!disk.contains(nonce), "{disk}");
        assert!(disk.len() < 40 * 1024);
        let mut report = String::new();
        append_operation_logs(&mut report, &entries);
        assert!(report.contains("operationLogEntries=1"));
        assert!(report.contains("setup broker start failed"));
        assert!(!report.contains(profile));
        assert!(!report.contains(nonce));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn operation_log_rotation_is_bounded_and_retains_recent_failures() {
        let root = env::temp_dir().join(format!(
            "mactype-operation-log-rotation-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        for index in 0..120 {
            operation_log::record_operation_failure_at(
                &root,
                &OperationFailure {
                    operation: "install".to_owned(),
                    stage: format!("fixture-{index}"),
                    error_chain: format!("failure-{index}: {}", "x".repeat(24 * 1024)),
                    broker_exit_code: Some(21),
                    channel_failure: None,
                    rollback: "not-applicable".to_owned(),
                    final_state: "legacy=absent; modern=absent".to_owned(),
                },
                &[],
            )
            .unwrap();
        }

        let files = fs::read_dir(&root)
            .unwrap()
            .map(|entry| entry.unwrap())
            .collect::<Vec<_>>();
        assert!(
            files.len() <= 5,
            "rotation created too many files: {files:?}"
        );
        assert!(files
            .iter()
            .all(|entry| entry.metadata().unwrap().len() <= 512 * 1024));
        let recent = operation_log::read_recent_operation_logs_at(&root, 1).unwrap();
        assert_eq!(recent.len(), 1);
        assert_eq!(recent[0].stage, "fixture-119");
        fs::remove_dir_all(root).unwrap();
    }
}
