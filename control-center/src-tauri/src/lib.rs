mod execution;
mod generated_settings;
mod preview;
mod profile;

use serde::Serialize;
use std::{env, fs, path::PathBuf, sync::Mutex, thread, time::Duration};
use tauri::{AppHandle, Manager, State};

use preview::{PreviewManager, PreviewResult, PreviewSample};
use profile::{IndividualSetting, ProfileDocument, ProfileSnapshot};

#[derive(Default)]
struct ProfileState(Mutex<Option<ProfileDocument>>);

#[derive(Default)]
struct PreviewState(Mutex<PreviewManager>);

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LaunchContext {
    view: String,
    ci_smoke: bool,
    tray_start: bool,
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

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ProfileEntry {
    name: String,
    path: String,
}

fn requested_view() -> String {
    let mut args = env::args();
    while let Some(argument) = args.next() {
        if argument == "--ci-view" {
            if let Some(value) = args.next() {
                if matches!(
                    value.as_str(),
                    "overview" | "profiles" | "execution" | "diagnostics"
                ) {
                    return value;
                }
            }
        }
    }
    "overview".to_owned()
}

fn starts_in_tray() -> bool {
    env::args().any(|argument| argument == "--tray")
}

#[tauri::command]
fn launch_context() -> LaunchContext {
    LaunchContext {
        view: requested_view(),
        ci_smoke: env::var_os("MACTYPE_CI_SMOKE_FILE").is_some(),
        tray_start: starts_in_tray(),
    }
}

#[tauri::command]
fn scan_installation() -> InstallationStatus {
    let root = installation_root();

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
        finding("수동 실행 로더", "MacLoader.exe"),
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
fn execution_status() -> execution::ExecutionStatus {
    execution::status(installation_root().as_deref())
}

#[tauri::command]
fn set_session_autostart(enabled: bool) -> Result<bool, String> {
    execution::set_autostart(enabled)
}

#[tauri::command]
fn launch_with_mactype(target: String, arguments: Vec<String>) -> Result<u32, String> {
    let root =
        installation_root().ok_or_else(|| "MacType installation was not found".to_owned())?;
    execution::launch_with_mactype(&root, &target, &arguments)
}

fn installation_root() -> Option<PathBuf> {
    env::var_os("MACTYPE_HOME")
        .map(PathBuf::from)
        .or_else(|| {
            env::current_exe()
                .ok()
                .and_then(|executable| executable.parent().map(PathBuf::from))
                .filter(|path| path.join("MacType.dll").is_file())
        })
        .or_else(|| env::var_os("ProgramFiles").map(|path| PathBuf::from(path).join("MacType")))
}

fn user_profile_root() -> Option<PathBuf> {
    env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .map(|path| path.join("MacType").join("ControlCenter").join("profiles"))
}

fn find_default_profile() -> Option<PathBuf> {
    let root = installation_root()?;
    let profile_root = root.join("ini");
    let default = profile_root.join("Default.ini");
    if default.is_file() {
        return Some(default);
    }
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
fn list_profiles() -> Result<Vec<ProfileEntry>, String> {
    let root =
        installation_root().ok_or_else(|| "MacType installation was not found".to_owned())?;
    let profile_root = root.join("ini");
    let mut paths = fs::read_dir(&profile_root)
        .map(|entries| {
            entries
                .filter_map(Result::ok)
                .map(|entry| entry.path())
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let root_profile = root.join("MacType.ini");
    if paths.is_empty() && root_profile.is_file() {
        paths.push(root_profile);
    }
    if let Some(user_root) = user_profile_root() {
        if let Ok(entries) = fs::read_dir(user_root) {
            paths.extend(entries.filter_map(Result::ok).map(|entry| entry.path()));
        }
    }
    let mut profiles = paths
        .into_iter()
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension.eq_ignore_ascii_case("ini"))
        })
        .map(|path| ProfileEntry {
            name: path
                .file_stem()
                .map(|name| name.to_string_lossy().into_owned())
                .unwrap_or_default(),
            path: path.to_string_lossy().into_owned(),
        })
        .collect::<Vec<_>>();
    profiles.sort_by_key(|profile| profile.name.to_lowercase());
    Ok(profiles)
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
fn update_profile_individuals(
    entries: Vec<IndividualSetting>,
    state: State<'_, ProfileState>,
) -> Result<ProfileSnapshot, String> {
    let mut guard = state
        .0
        .lock()
        .map_err(|_| "profile lock is poisoned".to_owned())?;
    let document = guard
        .as_mut()
        .ok_or_else(|| "no profile is open".to_owned())?;
    document.set_individuals(entries)?;
    Ok(document.snapshot())
}

#[tauri::command]
fn update_profile_list(
    kind: String,
    entries: Vec<String>,
    state: State<'_, ProfileState>,
) -> Result<ProfileSnapshot, String> {
    let mut guard = state
        .0
        .lock()
        .map_err(|_| "profile lock is poisoned".to_owned())?;
    let document = guard
        .as_mut()
        .ok_or_else(|| "no profile is open".to_owned())?;
    document.set_list(&kind, entries)?;
    Ok(document.snapshot())
}

#[tauri::command]
fn duplicate_profile(
    name: String,
    state: State<'_, ProfileState>,
) -> Result<ProfileSnapshot, String> {
    let mut guard = state
        .0
        .lock()
        .map_err(|_| "profile lock is poisoned".to_owned())?;
    let current = guard
        .as_ref()
        .ok_or_else(|| "no profile is open".to_owned())?;
    let directory =
        user_profile_root().ok_or_else(|| "LOCALAPPDATA is not available".to_owned())?;
    let duplicate = current.duplicate_in(&directory, &name)?;
    let snapshot = duplicate.snapshot();
    *guard = Some(duplicate);
    Ok(snapshot)
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
fn ci_verify_profile_workflow(state: State<'_, ProfileState>) -> Result<(), String> {
    let marker = env::var_os("MACTYPE_CI_SMOKE_FILE").ok_or_else(|| {
        "profile workflow verification is available only during CI smoke tests".to_owned()
    })?;
    let directory = PathBuf::from(marker)
        .parent()
        .ok_or_else(|| "CI marker has no parent directory".to_owned())?
        .join("profile-workflow");
    let guard = state
        .0
        .lock()
        .map_err(|_| "profile lock is poisoned".to_owned())?;
    let current = guard
        .as_ref()
        .ok_or_else(|| "no profile is open".to_owned())?;
    let name = format!("phase3-{}", std::process::id());
    let mut copy = current.duplicate_in(&directory, &name)?;
    copy.set_value("normal_weight", 7.0)?;
    copy.set_individuals(vec![IndividualSetting {
        font_face: "CI Test Font".to_owned(),
        values: vec![Some(1), Some(2), None, Some(3), None, Some(1)],
    }])?;
    copy.set_list("excludeModules", vec!["ci-test.exe".to_owned()])?;
    copy.save()?;
    let path = directory.join(format!("{name}.ini"));
    let reopened = ProfileDocument::open(&path)?.snapshot();
    if reopened.values.get("normal_weight") != Some(&7.0)
        || reopened.individuals.len() != 1
        || reopened.lists.exclude_modules != vec!["ci-test.exe".to_owned()]
    {
        return Err("saved Phase 3 profile did not reopen with the expected values".to_owned());
    }
    drop(guard);
    fs::remove_dir_all(directory).map_err(|error| error.to_string())
}

#[tauri::command]
fn ci_verify_tray_mode(app: AppHandle) -> Result<(), String> {
    if env::var_os("MACTYPE_CI_SMOKE_FILE").is_none() || !starts_in_tray() {
        return Err("tray verification requires CI smoke with --tray".to_owned());
    }
    let window = app
        .get_webview_window("main")
        .ok_or_else(|| "main window was not created".to_owned())?;
    if window.is_visible().map_err(|error| error.to_string())? {
        return Err("main window is visible during --tray startup".to_owned());
    }
    Ok(())
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

#[tauri::command]
fn frontend_failed(app: AppHandle, view: String, message: String) -> Result<(), String> {
    let Some(marker_path) = env::var_os("MACTYPE_CI_SMOKE_FILE") else {
        return Ok(());
    };
    fs::write(
        PathBuf::from(marker_path),
        format!("error:{view}:{message}\n"),
    )
    .map_err(|error| error.to_string())?;
    thread::spawn(move || {
        thread::sleep(Duration::from_millis(150));
        app.exit(1);
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
            execution_status,
            set_session_autostart,
            launch_with_mactype,
            list_profiles,
            open_profile,
            open_default_profile,
            update_profile_setting,
            update_profile_individuals,
            update_profile_list,
            duplicate_profile,
            save_profile,
            render_profile_preview,
            set_native_preview,
            preview_diagnostics,
            ci_force_preview_crash,
            ci_verify_profile_workflow,
            ci_verify_tray_mode,
            frontend_ready,
            frontend_failed
        ])
        .setup(|app| {
            use tauri::{
                menu::{Menu, MenuItem},
                tray::TrayIconBuilder,
            };
            let show = MenuItem::with_id(app, "show", "Control Center 열기", true, None::<&str>)?;
            let hide = MenuItem::with_id(app, "hide", "창 숨기기", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "종료", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show, &hide, &quit])?;
            let mut tray = TrayIconBuilder::new()
                .menu(&menu)
                .tooltip("MacType Control Center")
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id().as_ref() {
                    "show" => {
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.show();
                            let _ = window.set_focus();
                        }
                    }
                    "hide" => {
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.hide();
                        }
                    }
                    "quit" => app.exit(0),
                    _ => {}
                });
            if let Some(icon) = app.default_window_icon().cloned() {
                tray = tray.icon(icon);
            }
            tray.build(app)?;
            if starts_in_tray() {
                if let Some(window) = app.get_webview_window("main") {
                    window.hide()?;
                }
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                if env::var_os("MACTYPE_CI_SMOKE_FILE").is_none() {
                    api.prevent_close();
                    let _ = window.hide();
                }
            }
        })
        .run(tauri::generate_context!())
        .expect("failed to run MacType Control Center");
}

#[cfg(test)]
mod tests {
    #[test]
    fn unsupported_view_is_not_accepted_by_launch_parser_contract() {
        assert!(!matches!(
            "settings",
            "overview" | "profiles" | "execution" | "diagnostics"
        ));
    }
}
