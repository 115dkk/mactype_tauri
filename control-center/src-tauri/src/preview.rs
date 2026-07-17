use std::{collections::BTreeMap, sync::Mutex};
use tauri::{AppHandle, State};

mod commands;
mod helper;
mod installation;
mod protocol;
mod render;
mod state;

use helper::PreviewManager;
pub(crate) use installation::InstallationStatus;
pub(crate) use render::{PreviewResult, PreviewSample};

#[derive(Default)]
pub(crate) struct PreviewState(Mutex<PreviewManager>);

pub(crate) struct PreviewDiagnosticSnapshot {
    pub(crate) status: InstallationStatus,
    pub(crate) entries: Vec<String>,
}

#[tauri::command]
pub(crate) fn scan_installation(
    state: State<'_, PreviewState>,
) -> Result<InstallationStatus, String> {
    commands::scan_installation(state.inner())
}

#[tauri::command]
pub(crate) fn rediscover_installation(
    state: State<'_, PreviewState>,
) -> Result<InstallationStatus, String> {
    commands::rediscover_installation(state.inner())
}

#[tauri::command]
pub(crate) fn reconnect_preview(
    state: State<'_, PreviewState>,
) -> Result<InstallationStatus, String> {
    commands::reconnect_preview(state.inner())
}

#[tauri::command]
pub(crate) fn render_profile_preview(
    app: AppHandle,
    profile_path: String,
    overrides: BTreeMap<String, f64>,
    sample: PreviewSample,
    state: State<'_, PreviewState>,
) -> Result<PreviewResult, String> {
    commands::render_profile_preview(app, profile_path, overrides, sample, state.inner())
}

#[tauri::command]
pub(crate) fn set_native_preview(
    visible: bool,
    state: State<'_, PreviewState>,
) -> Result<bool, String> {
    commands::set_native_preview_visible(visible, state.inner())
}

#[tauri::command]
pub(crate) fn preview_diagnostics(state: State<'_, PreviewState>) -> Result<Vec<String>, String> {
    commands::preview_diagnostics(state.inner())
}

#[tauri::command]
pub(crate) fn ci_force_preview_crash(state: State<'_, PreviewState>) -> Result<(), String> {
    commands::ci_force_preview_crash(state.inner())
}
