import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import { settingsSchema } from "../generated/settings";
import { fallbackStatus, type AdvancedProfile, type AppliedProfile, type ExecutionStatus, type IndividualSetting, type InstallationStatus, type LaunchContext, type PreviewRequest, type PreviewResult, type ProfileEntry, type ProfileSnapshot, type SessionTarget, type ViewId } from "./model";
import type { Locale } from "../i18n/i18n";

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
  individuals: [{ fontFace: "Segoe UI", values: [1, 2, null, null, null, 1] }],
  lists: { excludeFonts: ["Raster Fonts"], includeFonts: [], excludeModules: ["fontview.exe"], includeModules: [], unloadDlls: [], excludeSubstitutionModules: [] },
  advanced: { shadow: null, lcdFilterWeight: null, pixelLayout: null, displayAffinity: [], fontSubstitutes: [], infinalityGammaCorrection: [0, 100], infinalityFilterParams: [11, 22, 38, 22, 11] },
};

export async function loadLaunchContext(): Promise<LaunchContext> {
  const requested = new URLSearchParams(window.location.search).get("view");
  if (!isTauriRuntime()) {
    return {
      view: requested === "profiles" || requested === "execution" || requested === "diagnostics" ? requested : "overview",
      ciSmoke: false,
      trayStart: false,
    };
  }
  return invoke<LaunchContext>("launch_context");
}

export async function setApplicationLocale(locale: Locale): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("set_application_locale", { locale });
}

export async function loadExecutionStatus(): Promise<ExecutionStatus> {
  if (!isTauriRuntime()) {
    return { trayAvailable: true, autoStart: false, manualLauncherAvailable: true, legacyServiceDetected: true, legacyServiceRunning: true, registryModeDetected: false, systemModesSupported: false, systemModeNote: "시스템 모드는 안전성 검토 결과 읽기 전용으로 표시됩니다.", injectionReady: true, activeProfile: fallbackProfile.path, sessionTargets: [] };
  }
  return invoke<ExecutionStatus>("execution_status");
}

export async function setSessionAutostart(enabled: boolean): Promise<boolean> {
  if (!isTauriRuntime()) return enabled;
  return invoke<boolean>("set_session_autostart", { enabled });
}

export async function launchTargetWithMactype(target: string, arguments_: ReadonlyArray<string>): Promise<number> {
  if (!isTauriRuntime()) return 4242;
  return invoke<number>("launch_with_mactype", { target, arguments: arguments_ });
}

export async function scanInstallation(): Promise<InstallationStatus | null> {
  if (!isTauriRuntime()) return null;
  return invoke<InstallationStatus>("scan_installation");
}

export async function applyOpenProfile(): Promise<AppliedProfile> {
  if (!isTauriRuntime()) return { sourceProfile: fallbackProfile.path, runtimeRoot: "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\runtime\\generations\\gallery" };
  return invoke<AppliedProfile>("apply_open_profile");
}

export async function registerSessionTarget(target: string, arguments_: ReadonlyArray<string>): Promise<ReadonlyArray<SessionTarget>> {
  if (!isTauriRuntime()) return [{ target, arguments: arguments_ }];
  return invoke<SessionTarget[]>("register_session_target", { target, arguments: arguments_ });
}

export async function removeSessionTarget(target: string): Promise<ReadonlyArray<SessionTarget>> {
  if (!isTauriRuntime()) return [];
  return invoke<SessionTarget[]>("remove_session_target", { target });
}

export async function launchRegisteredTargets(): Promise<ReadonlyArray<number>> {
  if (!isTauriRuntime()) return [4242];
  return invoke<number[]>("launch_registered_targets");
}

export async function rediscoverInstallation(): Promise<InstallationStatus> {
  if (!isTauriRuntime()) return { ...fallbackStatus, state: "ready" };
  return invoke<InstallationStatus>("rediscover_installation");
}

export async function reconnectPreview(): Promise<InstallationStatus> {
  if (!isTauriRuntime()) {
    return {
      ...fallbackStatus,
      state: "ready",
      findings: fallbackStatus.findings.map((finding) => finding.label === "preview" ? { ...finding, value: "connected", ok: true } : finding),
    };
  }
  return invoke<InstallationStatus>("reconnect_preview");
}

export async function loadDiagnosticReport(): Promise<string> {
  if (!isTauriRuntime()) {
    return `MacType Control Center diagnostics\ncontrolCenterVersion=0.1.0\ncoreVersion=${fallbackStatus.coreVersion}\n`;
  }
  return invoke<string>("diagnostic_report");
}

export async function exportDiagnostics(): Promise<string> {
  if (!isTauriRuntime()) return "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\logs\\diagnostics-gallery.txt";
  return invoke<string>("export_diagnostics");
}

export async function copyDiagnostics(): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("copy_diagnostics");
}

export async function openLogFolder(): Promise<string> {
  if (!isTauriRuntime()) return "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\logs";
  return invoke<string>("open_log_folder");
}

export async function openDefaultProfile(): Promise<ProfileSnapshot | null> {
  if (!isTauriRuntime()) return fallbackProfile;
  return invoke<ProfileSnapshot | null>("open_default_profile");
}

export async function listProfiles(): Promise<ReadonlyArray<ProfileEntry>> {
  if (!isTauriRuntime()) return [{ name: "Default", path: fallbackProfile.path }];
  return invoke<ProfileEntry[]>("list_profiles");
}

export async function openProfile(path: string): Promise<ProfileSnapshot> {
  if (!isTauriRuntime()) return { ...fallbackProfile, path };
  return invoke<ProfileSnapshot>("open_profile", { path });
}

export async function duplicateProfile(name: string): Promise<ProfileSnapshot> {
  if (!isTauriRuntime()) return { ...fallbackProfile, path: `C:\\Program Files\\MacType\\ini\\${name}.ini` };
  return invoke<ProfileSnapshot>("duplicate_profile", { name });
}

export async function updateProfileSetting(settingId: string, value: number): Promise<ProfileSnapshot | null> {
  if (!isTauriRuntime()) return null;
  return invoke<ProfileSnapshot>("update_profile_setting", { settingId, value });
}

export async function updateProfileIndividuals(entries: ReadonlyArray<IndividualSetting>): Promise<ProfileSnapshot | null> {
  if (!isTauriRuntime()) return null;
  return invoke<ProfileSnapshot>("update_profile_individuals", { entries });
}

export async function updateProfileList(kind: string, entries: ReadonlyArray<string>): Promise<ProfileSnapshot | null> {
  if (!isTauriRuntime()) return null;
  return invoke<ProfileSnapshot>("update_profile_list", { kind, entries });
}

export async function updateProfileAdvanced(advanced: AdvancedProfile): Promise<ProfileSnapshot | null> {
  if (!isTauriRuntime()) return null;
  return invoke<ProfileSnapshot>("update_profile_advanced", { advanced });
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

export async function verifyProfileWorkflowForCi(): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("ci_verify_profile_workflow");
}

export async function verifyInjectionWorkflowForCi(): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("ci_verify_injection_workflow");
}

export async function verifyTrayModeForCi(): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("ci_verify_tray_mode");
}

export async function reportFrontendReady(view: ViewId): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("frontend_ready", { view });
}

export async function reportFrontendFailure(view: ViewId, message: string): Promise<void> {
  if (!isTauriRuntime()) return;
  await invoke("frontend_failed", { view, message });
}
