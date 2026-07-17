import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import { open, save } from "@tauri-apps/plugin-dialog";
import type { Locale } from "../../i18n/i18n";
import type {
  AdvancedProfile,
  AppliedProfile,
  ExecutionStatus,
  IndividualSetting,
  InstallationStatus,
  LegacyProfileCandidate,
  SystemServiceAction,
  LaunchContext,
  PreviewRequest,
  PreviewResult,
  ProfileEntry,
  ProfileSnapshot,
  SessionTarget,
  ViewId,
} from "../model";
import type { ControlCenterRuntimeAdapter } from "../runtimeAdapter";

export const tauriRuntimeAdapter: ControlCenterRuntimeAdapter = {
  loadLaunchContext: () => invoke<LaunchContext>("launch_context"),

  async setApplicationLocale(locale: Locale): Promise<void> {
    await invoke("set_application_locale", { locale });
  },

  loadExecutionStatus: () => invoke<ExecutionStatus>("execution_status"),
  manageSystemService: (action: SystemServiceAction) => invoke<ExecutionStatus>("manage_system_service", { action }),
  revealSystemService: () => invoke<void>("reveal_system_service"),

  async pickExecutable(filterName: string): Promise<string | null> {
    const selected = await open({
      directory: false,
      multiple: false,
      filters: [{ name: filterName, extensions: ["exe"] }],
    });
    return typeof selected === "string" ? selected : null;
  },

  async pickIniProfile(filterName: string): Promise<string | null> {
    const selected = await open({
      directory: false,
      multiple: false,
      filters: [{ name: filterName, extensions: ["ini"] }],
    });
    return typeof selected === "string" ? selected : null;
  },

  async pickIniExportPath(filterName: string, defaultName: string): Promise<string | null> {
    const selected = await save({
      defaultPath: defaultName,
      filters: [{ name: filterName, extensions: ["ini"] }],
    });
    return typeof selected === "string" ? selected : null;
  },

  loadInstalledFontFamilies: () => invoke<string[]>("installed_font_families"),
  setSessionAutostart: (enabled: boolean) => invoke<boolean>("set_session_autostart", { enabled }),
  launchTargetWithMactype: (target: string, arguments_: ReadonlyArray<string>) => invoke<number>("launch_with_mactype", { target, arguments: arguments_ }),
  scanInstallation: () => invoke<InstallationStatus>("scan_installation"),
  applyOpenProfile: () => invoke<AppliedProfile>("apply_open_profile"),
  registerSessionTarget: (target: string, arguments_: ReadonlyArray<string>) => invoke<SessionTarget[]>("register_session_target", { target, arguments: arguments_ }),
  removeSessionTarget: (target: string) => invoke<SessionTarget[]>("remove_session_target", { target }),
  launchRegisteredTargets: () => invoke<number[]>("launch_registered_targets"),
  rediscoverInstallation: () => invoke<InstallationStatus>("rediscover_installation"),
  reconnectPreview: () => invoke<InstallationStatus>("reconnect_preview"),
  loadDiagnosticReport: () => invoke<string>("diagnostic_report"),
  exportDiagnostics: () => invoke<string>("export_diagnostics"),

  async copyDiagnostics(): Promise<void> {
    await invoke("copy_diagnostics");
  },

  openLogFolder: () => invoke<string>("open_log_folder"),
  openDefaultProfile: () => invoke<ProfileSnapshot | null>("open_default_profile"),
  currentProfile: () => invoke<ProfileSnapshot | null>("current_profile"),
  discoverLegacyProfile: () => invoke<LegacyProfileCandidate | null>("discover_legacy_profile"),
  importProfile: (path: string) => invoke<ProfileSnapshot>("import_profile", { path }),
  listProfiles: () => invoke<ProfileEntry[]>("list_profiles"),
  openProfile: (path: string) => invoke<ProfileSnapshot>("open_profile", { path }),
  duplicateProfile: (name: string) => invoke<ProfileSnapshot>("duplicate_profile", { name }),
  updateProfileSetting: (settingId: string, value: number) => invoke<ProfileSnapshot>("update_profile_setting", { settingId, value }),
  updateProfileIndividuals: (entries: ReadonlyArray<IndividualSetting>) => invoke<ProfileSnapshot>("update_profile_individuals", { entries }),
  updateProfileList: (kind: string, entries: ReadonlyArray<string>) => invoke<ProfileSnapshot>("update_profile_list", { kind, entries }),
  updateProfileAdvanced: (advanced: AdvancedProfile) => invoke<ProfileSnapshot>("update_profile_advanced", { advanced }),
  undoProfile: () => invoke<ProfileSnapshot>("undo_profile"),
  redoProfile: () => invoke<ProfileSnapshot>("redo_profile"),
  discardProfileChanges: () => invoke<ProfileSnapshot>("discard_profile_changes"),
  exportProfile: (path: string) => invoke<string>("export_profile", { path }),
  revealProfileFile: () => invoke<string>("reveal_profile_file"),
  saveProfile: () => invoke<ProfileSnapshot>("save_profile"),
  renderProfilePreview: (request: PreviewRequest) => invoke<PreviewResult>("render_profile_preview", {
    profilePath: request.profilePath,
    overrides: request.overrides,
    sample: request.sample,
  }),
  setNativePreview: (visible: boolean) => invoke<boolean>("set_native_preview", { visible }),
  previewImageUrl: (path: string) => convertFileSrc(path),
  loadPreviewDiagnostics: () => invoke<string[]>("preview_diagnostics"),

  async forcePreviewCrashForCi(): Promise<void> {
    await invoke("ci_force_preview_crash");
  },

  async verifyProfileWorkflowForCi(): Promise<void> {
    await invoke("ci_verify_profile_workflow");
  },

  async verifyInjectionWorkflowForCi(): Promise<void> {
    await invoke("ci_verify_injection_workflow");
  },

  async verifyTrayModeForCi(): Promise<void> {
    await invoke("ci_verify_tray_mode");
  },

  async reportFrontendReady(view: ViewId): Promise<void> {
    await invoke("frontend_ready", { view });
  },

  async reportFrontendFailure(view: ViewId, message: string): Promise<void> {
    await invoke("frontend_failed", { view, message });
  },
};
