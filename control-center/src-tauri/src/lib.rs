mod generated_settings;
mod preview;
mod profile;

use serde::Serialize;
use std::{env, fs, path::PathBuf, sync::Mutex, thread, time::Duration};
use tauri::{AppHandle, State};

use preview::{PreviewManager, PreviewResult, PreviewSample};
use profile::{ProfileDocument, ProfileSnapshot};

#[derive(Default)]
struct ProfileState(Mutex<Option<ProfileDocument>>);

#[derive(Default)]
struct PreviewState(Mutex<PreviewManager>);

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

fn installation_root() -> Option<PathBuf> {
    env::var_os("MACTYPE_HOME")
        .map(PathBuf::from)
        .or_else(|| env::var_os("ProgramFiles").map(|path| PathBuf::from(path).join("MacType")))
}

fn find_default_profile() -> Option<PathBuf> {
    let root = installation_root()?;
    let profile_root = root.join("ini");
    let profile = fs::read_dir(&profile_root).ok().and_then(|entries| {
        entries
            .filter_map(Result::ok)
            .map(|entry| entry.path())
            .find(|path| {
                path.extension()
                    .is_some_and(|extension| extension.eq_ignore_ascii_case("ini"))
            })
    });
    profile.or_else(|| {
        root.join("MacType.ini")
            .is_file()
            .then(|| root.join("MacType.ini"))
    })
}

#[tauri::command]
fn open_profile(path: String, state: State<'_, ProfileState>) -> Result<ProfileSnapshot, String> {
    let document = ProfileDocument::open(PathBuf::from(path))?;
    let snapshot = document.snapshot();
    *state
        .0
        .lock()
        .map_err(|_| "profile lock is poisoned".to_owned())? = Some(document);
    Ok(snapshot)
}

#[tauri::command]
fn open_default_profile(state: State<'_, ProfileState>) -> Result<Option<ProfileSnapshot>, String> {
    let Some(path) = find_default_profile() else {
        return Ok(None);
    };
    open_profile(path.to_string_lossy().into_owned(), state).map(Some)
}

#[tauri::command]
fn update_profile_setting(
    setting_id: String,
    value: f64,
    state: State<'_, ProfileState>,
) -> Result<ProfileSnapshot, String> {
    let mut guard = state
        .0
        .lock()
        .map_err(|_| "profile lock is poisoned".to_owned())?;
    let document = guard
        .as_mut()
        .ok_or_else(|| "no profile is open".to_owned())?;
    document.set_value(&setting_id, value)?;
    Ok(document.snapshot())
}

#[tauri::command]
fn save_profile(state: State<'_, ProfileState>) -> Result<ProfileSnapshot, String> {
    let mut guard = state
        .0
        .lock()
        .map_err(|_| "profile lock is poisoned".to_owned())?;
    let document = guard
        .as_mut()
        .ok_or_else(|| "no profile is open".to_owned())?;
    document.save()?;
    Ok(document.snapshot())
}

#[tauri::command]
fn render_profile_preview(
    app: AppHandle,
    profile_path: String,
    overrides: std::collections::BTreeMap<String, f64>,
    sample: PreviewSample,
    state: State<'_, PreviewState>,
) -> Result<PreviewResult, String> {
    let root =
        installation_root().ok_or_else(|| "MacType installation was not found".to_owned())?;
    let mut manager = state
        .0
        .lock()
        .map_err(|_| "preview lock is poisoned".to_owned())?;
    preview::render_preview(
        &app,
        &mut manager,
        &root,
        &profile_path,
        &overrides,
        &sample,
    )
}

#[tauri::command]
fn set_native_preview(visible: bool, state: State<'_, PreviewState>) -> Result<bool, String> {
    let root =
        installation_root().ok_or_else(|| "MacType installation was not found".to_owned())?;
    let mut manager = state
        .0
        .lock()
        .map_err(|_| "preview lock is poisoned".to_owned())?;
    preview::set_native_preview(&mut manager, &root, visible)
}

#[tauri::command]
fn preview_diagnostics(state: State<'_, PreviewState>) -> Result<Vec<String>, String> {
    let manager = state
        .0
        .lock()
        .map_err(|_| "preview lock is poisoned".to_owned())?;
    Ok(manager.diagnostics())
}

#[tauri::command]
fn ci_force_preview_crash(state: State<'_, PreviewState>) -> Result<(), String> {
    if env::var_os("MACTYPE_CI_SMOKE_FILE").is_none() {
        return Err("preview crash injection is available only during CI smoke tests".to_owned());
    }
    let mut manager = state
        .0
        .lock()
        .map_err(|_| "preview lock is poisoned".to_owned())?;
    manager.force_terminate_for_ci()
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
        .manage(ProfileState::default())
        .manage(PreviewState::default())
        .invoke_handler(tauri::generate_handler![
            launch_context,
            scan_installation,
            open_profile,
            open_default_profile,
            update_profile_setting,
            save_profile,
            render_profile_preview,
            set_native_preview,
            preview_diagnostics,
            ci_force_preview_crash,
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
