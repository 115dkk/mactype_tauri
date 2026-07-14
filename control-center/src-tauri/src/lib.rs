mod app;
mod diagnostics;
mod execution;
mod fonts;
mod generated_settings;
mod preview;
mod profile;

use std::{env, path::PathBuf};
use tauri::Manager;

pub(crate) fn installation_root() -> Option<PathBuf> {
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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(profile::ProfileState::default())
        .manage(preview::PreviewState::default())
        .invoke_handler(tauri::generate_handler![
            app::launch_context,
            fonts::installed_font_families,
            app::set_application_locale,
            preview::scan_installation,
            preview::rediscover_installation,
            preview::reconnect_preview,
            diagnostics::diagnostic_report,
            diagnostics::export_diagnostics,
            diagnostics::copy_diagnostics,
            diagnostics::open_log_folder,
            execution::execution_status,
            execution::set_session_autostart,
            execution::launch_with_mactype,
            execution::apply_open_profile,
            execution::register_session_target,
            execution::remove_session_target,
            execution::launch_registered_targets,
            profile::list_profiles,
            profile::open_profile,
            profile::open_default_profile,
            profile::update_profile_setting,
            profile::update_profile_individuals,
            profile::update_profile_list,
            profile::update_profile_advanced,
            profile::duplicate_profile,
            profile::save_profile,
            preview::render_profile_preview,
            preview::set_native_preview,
            preview::preview_diagnostics,
            preview::ci_force_preview_crash,
            profile::ci_verify_profile_workflow,
            execution::ci_verify_injection_workflow,
            app::ci_verify_tray_mode,
            app::frontend_ready,
            app::frontend_failed
        ])
        .setup(|app| {
            use tauri::{
                menu::{Menu, MenuItem},
                tray::TrayIconBuilder,
            };
            let (show_label, inject_label, hide_label, quit_label) = app::tray_menu_labels("ko");
            let show = MenuItem::with_id(app, "show", show_label, true, None::<&str>)?;
            let hide = MenuItem::with_id(app, "hide", hide_label, true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", quit_label, true, None::<&str>)?;
            let inject = MenuItem::with_id(app, "inject", inject_label, true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show, &inject, &hide, &quit])?;
            let mut tray = TrayIconBuilder::with_id("main")
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
                    "inject" => {
                        let _ = execution::launch_registered_targets();
                    }
                    "quit" => app.exit(0),
                    _ => {}
                });
            if let Some(icon) = app.default_window_icon().cloned() {
                tray = tray.icon(icon);
            }
            tray.build(app)?;
            if app::starts_in_tray() {
                if let Some(window) = app.get_webview_window("main") {
                    window.hide()?;
                }
                let _ = execution::launch_registered_targets();
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
