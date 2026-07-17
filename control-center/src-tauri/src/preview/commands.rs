use super::{
    installation::{collect_installation, InstallationStatus},
    render::{render_preview, set_native_preview, PreviewResult, PreviewSample},
    PreviewState,
};
use crate::installation_root;
use std::{collections::BTreeMap, env};
use tauri::AppHandle;

pub(super) fn scan_installation(state: &PreviewState) -> Result<InstallationStatus, String> {
    state.with_manager(|manager| Ok(collect_installation(manager, false)))
}

pub(super) fn rediscover_installation(state: &PreviewState) -> Result<InstallationStatus, String> {
    scan_installation(state)
}

pub(super) fn reconnect_preview(state: &PreviewState) -> Result<InstallationStatus, String> {
    state.with_manager(|manager| Ok(collect_installation(manager, true)))
}

pub(super) fn render_profile_preview(
    app: AppHandle,
    profile_path: String,
    overrides: BTreeMap<String, f64>,
    sample: PreviewSample,
    state: &PreviewState,
) -> Result<PreviewResult, String> {
    let root =
        installation_root().ok_or_else(|| "MacType installation was not found".to_owned())?;
    state.with_manager(|manager| {
        render_preview(&app, manager, &root, &profile_path, &overrides, &sample)
    })
}

pub(super) fn set_native_preview_visible(
    visible: bool,
    state: &PreviewState,
) -> Result<bool, String> {
    let root =
        installation_root().ok_or_else(|| "MacType installation was not found".to_owned())?;
    state.with_manager(|manager| set_native_preview(manager, &root, visible))
}

pub(super) fn preview_diagnostics(state: &PreviewState) -> Result<Vec<String>, String> {
    state.with_manager(|manager| Ok(manager.diagnostics()))
}

pub(super) fn ci_force_preview_crash(state: &PreviewState) -> Result<(), String> {
    if env::var_os("MACTYPE_CI_SMOKE_FILE").is_none() {
        return Err("preview crash injection is available only during CI smoke tests".to_owned());
    }
    state.with_manager(|manager| manager.force_terminate_for_ci())
}
