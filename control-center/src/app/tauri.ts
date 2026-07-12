import { invoke } from "@tauri-apps/api/core";
import type { InstallationStatus, LaunchContext, ViewId } from "./model";

function isTauriRuntime(): boolean {
  return "__TAURI_INTERNALS__" in window;
}

export async function loadLaunchContext(): Promise<LaunchContext> {
  const requested = new URLSearchParams(window.location.search).get("view");
  if (!isTauriRuntime()) {
    return {
      view: requested === "profiles" || requested === "diagnostics" ? requested : "overview",
      ciSmoke: false,
    };
  }
  return invoke<LaunchContext>("launch_context");
}

export async function scanInstallation(): Promise<InstallationStatus | null> {
  if (!isTauriRuntime()) return null;
  return invoke<InstallationStatus>("scan_installation");
}

export async function reportFrontendReady(view: ViewId): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("frontend_ready", { view });
}
