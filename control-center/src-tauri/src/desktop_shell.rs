use crate::{app, execution, single_instance};
use std::{env, error::Error, thread};
use tauri::{
    menu::{Menu, MenuItem},
    tray::TrayIconBuilder,
    App, AppHandle, Emitter, Manager, Window, WindowEvent, Wry,
};

enum TrayBackgroundAction {
    Login,
    Apply,
}

pub(crate) fn install(
    app: &mut App<Wry>,
    startup_gate: single_instance::StartupGate,
) -> Result<(), Box<dyn Error>> {
    install_tray(app)?;
    if app::starts_in_tray() {
        if let Some(window) = app.get_webview_window("main") {
            window.hide()?;
        }
        spawn_background(app.handle().clone(), TrayBackgroundAction::Login);
    }
    startup_gate.release()?;
    single_instance::write_ready_marker()?;
    Ok(())
}

pub(crate) fn handle_window_event(window: &Window<Wry>, event: &WindowEvent) {
    if let WindowEvent::CloseRequested { api, .. } = event {
        if env::var_os("MACTYPE_CI_SMOKE_FILE").is_none() {
            api.prevent_close();
            let _ = window.hide();
        }
    }
}

fn install_tray(app: &mut App<Wry>) -> tauri::Result<()> {
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
                let _ = app::restore_main_window(app);
            }
            "hide" => {
                if let Some(window) = app.get_webview_window("main") {
                    let _ = window.hide();
                }
            }
            "inject" => spawn_background(app.clone(), TrayBackgroundAction::Apply),
            "quit" => app.exit(0),
            _ => {}
        });
    if let Some(icon) = app.default_window_icon().cloned() {
        tray = tray.icon(icon);
    }
    tray.build(app)?;
    Ok(())
}

fn spawn_background(app: AppHandle<Wry>, action: TrayBackgroundAction) {
    thread::spawn(move || run_background(app, action));
}

fn run_background(app: AppHandle<Wry>, action: TrayBackgroundAction) {
    let result = match action {
        TrayBackgroundAction::Login => execution::observe_machine_on_tray_login()
            .and_then(|_| execution::launch_registered_targets().map(|_| ())),
        TrayBackgroundAction::Apply => execution::apply_system_injection_from_tray_menu()
            .and_then(|()| execution::launch_registered_targets().map(|_| ())),
    };
    if let Err(error) = result {
        eprintln!("tray background operation failed: {error}");
        let _ = app.emit("machine-integration-error", &error);
        let _ = app::restore_main_window(&app);
    }
}
