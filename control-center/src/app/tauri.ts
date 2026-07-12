import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import { settingsSchema } from "../generated/settings";
import type { InstallationStatus, LaunchContext, PreviewRequest, PreviewResult, ProfileSnapshot, ViewId } from "./model";

function isTauriRuntime(): boolean {
  return "__TAURI_INTERNALS__" in window;
}

const fallbackProfile: ProfileSnapshot = {
  path: "C:\\Program Files\\MacType\\ini\\Default.ini",
  encoding: "utf-8",
  bom: "none",
  lineEnding: "cr-lf",
  originalHash: "browser-gallery",
  values: Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])),
  dirtyKeys: [],
};

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

export async function openDefaultProfile(): Promise<ProfileSnapshot | null> {
  if (!isTauriRuntime()) return fallbackProfile;
  return invoke<ProfileSnapshot | null>("open_default_profile");
}

export async function updateProfileSetting(settingId: string, value: number): Promise<ProfileSnapshot | null> {
  if (!isTauriRuntime()) return null;
  return invoke<ProfileSnapshot>("update_profile_setting", { settingId, value });
}

export async function saveProfile(): Promise<ProfileSnapshot | null> {
  if (!isTauriRuntime()) return null;
  return invoke<ProfileSnapshot>("save_profile");
}

export async function renderProfilePreview(request: PreviewRequest): Promise<PreviewResult | null> {
  if (!isTauriRuntime()) return null;
  return invoke<PreviewResult>("render_profile_preview", {
    profilePath: request.profilePath,
    overrides: request.overrides,
    sample: request.sample,
  });
}

export async function setNativePreview(visible: boolean): Promise<boolean> {
  if (!isTauriRuntime()) return visible;
  return invoke<boolean>("set_native_preview", { visible });
}

export function previewImageUrl(path: string): string {
  return isTauriRuntime() ? convertFileSrc(path) : path;
}

export async function loadPreviewDiagnostics(): Promise<ReadonlyArray<string>> {
  if (!isTauriRuntime()) return [];
  return invoke<string[]>("preview_diagnostics");
}

export async function forcePreviewCrashForCi(): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("ci_force_preview_crash");
}

export async function reportFrontendReady(view: ViewId): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("frontend_ready", { view });
}
