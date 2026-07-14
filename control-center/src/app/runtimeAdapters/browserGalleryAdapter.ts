import { settingsSchema } from "../../generated/settings";
import {
  fallbackStatus,
  type AppliedProfile,
  type ExecutionStatus,
  type InstallationStatus,
  type LaunchContext,
  type ProfileEntry,
  type ProfileSnapshot,
  type SessionTarget,
} from "../model";
import type { ControlCenterRuntimeAdapter } from "../runtimeAdapter";

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

export const browserGalleryAdapter: ControlCenterRuntimeAdapter = {
  async loadLaunchContext(): Promise<LaunchContext> {
    const requested = new URLSearchParams(window.location.search).get("view");
    return {
      view: requested === "profiles" || requested === "execution" || requested === "diagnostics" ? requested : "overview",
      ciSmoke: false,
      trayStart: false,
    };
  },

  async setApplicationLocale(): Promise<void> {},

  async loadExecutionStatus(): Promise<ExecutionStatus> {
    return { trayAvailable: true, autoStart: false, manualLauncherAvailable: true, legacyServiceDetected: true, legacyServiceRunning: true, registryModeDetected: false, systemModesSupported: false, systemModeNote: "시스템 모드는 안전성 검토 결과 읽기 전용으로 표시됩니다.", injectionReady: true, activeProfile: fallbackProfile.path, sessionTargets: [] };
  },

  async pickExecutable(): Promise<string | null> {
    return "C:\\Windows\\System32\\notepad.exe";
  },

  async loadInstalledFontFamilies(): Promise<ReadonlyArray<string>> {
    return ["Segoe UI", "Arial", "Calibri", "Cambria", "Consolas", "맑은 고딕", "Microsoft YaHei UI", "Microsoft JhengHei UI", "Meiryo", "Tahoma"];
  },

  async setSessionAutostart(enabled: boolean): Promise<boolean> {
    return enabled;
  },

  async launchTargetWithMactype(): Promise<number> {
    return 4242;
  },

  async scanInstallation(): Promise<InstallationStatus | null> {
    return null;
  },

  async applyOpenProfile(): Promise<AppliedProfile> {
    return { sourceProfile: fallbackProfile.path, runtimeRoot: "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\runtime\\generations\\gallery" };
  },

  async registerSessionTarget(target: string, arguments_: ReadonlyArray<string>): Promise<ReadonlyArray<SessionTarget>> {
    return [{ target, arguments: arguments_ }];
  },

  async removeSessionTarget(): Promise<ReadonlyArray<SessionTarget>> {
    return [];
  },

  async launchRegisteredTargets(): Promise<ReadonlyArray<number>> {
    return [4242];
  },

  async rediscoverInstallation(): Promise<InstallationStatus> {
    return { ...fallbackStatus, state: "ready" };
  },

  async reconnectPreview(): Promise<InstallationStatus> {
    return {
      ...fallbackStatus,
      state: "ready",
      findings: fallbackStatus.findings.map((finding) => finding.label === "preview" ? { ...finding, value: "connected", ok: true } : finding),
    };
  },

  async loadDiagnosticReport(): Promise<string> {
    return `MacType Control Center diagnostics\ncontrolCenterVersion=0.1.0\ncoreVersion=${fallbackStatus.coreVersion}\n`;
  },

  async exportDiagnostics(): Promise<string> {
    return "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\logs\\diagnostics-gallery.txt";
  },

  async copyDiagnostics(): Promise<void> {},

  async openLogFolder(): Promise<string> {
    return "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\logs";
  },

  async openDefaultProfile(): Promise<ProfileSnapshot | null> {
    return fallbackProfile;
  },

  async listProfiles(): Promise<ReadonlyArray<ProfileEntry>> {
    return [{ name: "Default", path: fallbackProfile.path }];
  },

  async openProfile(path: string): Promise<ProfileSnapshot> {
    return { ...fallbackProfile, path };
  },

  async duplicateProfile(name: string): Promise<ProfileSnapshot> {
    return { ...fallbackProfile, path: `C:\\Program Files\\MacType\\ini\\${name}.ini` };
  },

  async updateProfileSetting(): Promise<ProfileSnapshot | null> {
    return null;
  },

  async updateProfileIndividuals(): Promise<ProfileSnapshot | null> {
    return null;
  },

  async updateProfileList(): Promise<ProfileSnapshot | null> {
    return null;
  },

  async updateProfileAdvanced(): Promise<ProfileSnapshot | null> {
    return null;
  },

  async saveProfile(): Promise<ProfileSnapshot | null> {
    return null;
  },

  async renderProfilePreview(): Promise<null> {
    return null;
  },

  async setNativePreview(visible: boolean): Promise<boolean> {
    return visible;
  },

  previewImageUrl(path: string): string {
    return path;
  },

  async loadPreviewDiagnostics(): Promise<ReadonlyArray<string>> {
    return [];
  },

  async forcePreviewCrashForCi(): Promise<void> {},
  async verifyProfileWorkflowForCi(): Promise<void> {},
  async verifyInjectionWorkflowForCi(): Promise<void> {},
  async verifyTrayModeForCi(): Promise<void> {},
  async reportFrontendReady(): Promise<void> {},
  async reportFrontendFailure(): Promise<void> {},
};
