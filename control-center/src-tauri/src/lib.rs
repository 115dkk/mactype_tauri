use serde::Serialize;
use std::{env, fs, path::PathBuf, thread, time::Duration};
use tauri::AppHandle;

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LaunchContext {
    view: String,
    ci_smoke: bool,
}

#[derive(Serialize)]
struct Finding {
    label: String,
    value: String,
    ok: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct InstallationStatus {
    state: String,
    root: Option<String>,
    core_version: Option<String>,
    findings: Vec<Finding>,
}

fn requested_view() -> String {
    let mut args = env::args();
    while let Some(argument) = args.next() {
        if argument == "--ci-view" {
            if let Some(value) = args.next() {
                if matches!(value.as_str(), "overview" | "profiles" | "diagnostics") {
                    return value;
                }
            }
        }
    }
    "overview".to_owned()
}

#[tauri::command]
fn launch_context() -> LaunchContext {
    LaunchContext {
        view: requested_view(),
        ci_smoke: env::var_os("MACTYPE_CI_SMOKE_FILE").is_some(),
    }
}

#[tauri::command]
fn scan_installation() -> InstallationStatus {
    let root = env::var_os("MACTYPE_HOME")
        .map(PathBuf::from)
        .or_else(|| env::var_os("ProgramFiles").map(|path| PathBuf::from(path).join("MacType")));

    let finding = |label: &str, file: &str| {
        let ok = root.as_ref().is_some_and(|path| path.join(file).is_file());
        Finding {
            label: label.to_owned(),
            value: file.to_owned(),
            ok,
        }
    };

    let findings = vec![
        finding("32비트 코어", "MacType.dll"),
        finding("64비트 코어", "MacType64.dll"),
        finding("32비트 EasyHook", "EasyHK32.dll"),
    ];
    let ready = findings.iter().all(|item| item.ok);

    InstallationStatus {
        state: if ready { "ready" } else { "incomplete" }.to_owned(),
        root: root.map(|path| path.to_string_lossy().into_owned()),
        core_version: None,
        findings,
    }
}

#[tauri::command]
fn frontend_ready(app: AppHandle, view: String) -> Result<(), String> {
    let Some(marker_path) = env::var_os("MACTYPE_CI_SMOKE_FILE") else {
        return Ok(());
    };
    let marker = PathBuf::from(marker_path);
    fs::write(&marker, format!("ready:{view}\n")).map_err(|error| error.to_string())?;
    thread::spawn(move || {
        thread::sleep(Duration::from_millis(150));
        app.exit(0);
    });
    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            launch_context,
            scan_installation,
            frontend_ready
        ])
        .run(tauri::generate_context!())
        .expect("failed to run MacType Control Center");
}

#[cfg(test)]
mod tests {
    #[test]
    fn unsupported_view_is_not_accepted_by_launch_parser_contract() {
        assert!(!matches!(
            "settings",
            "overview" | "profiles" | "diagnostics"
        ));
    }
}
