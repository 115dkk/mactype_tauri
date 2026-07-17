use crate::preview::{PreviewDiagnosticSnapshot, PreviewState};
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
    Ok(diagnostic_report_text(state.diagnostic_snapshot()?))
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
}
