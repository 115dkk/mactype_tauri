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
  loadLaunchContext(): Promise<LaunchContext> {
    const requested = new URLSearchParams(window.location.search).get("view");
    return Promise.resolve<LaunchContext>({
      view: requested === "profiles" || requested === "execution" || requested === "diagnostics" ? requested : "overview",
      ciSmoke: false,
      trayStart: false,
    });
  },

  setApplicationLocale(): Promise<void> {
    return Promise.resolve();
  },

  loadExecutionStatus(): Promise<ExecutionStatus> {
    return Promise.resolve<ExecutionStatus>({ trayAvailable: true, autoStart: false, manualLauncherAvailable: true, legacyServiceDetected: true, legacyServiceRunning: true, registryModeDetected: false, systemModesSupported: false, systemModeNote: "시스템 모드는 안전성 검토 결과 읽기 전용으로 표시됩니다.", injectionReady: true, activeProfile: fallbackProfile.path, sessionTargets: [] });
  },

  pickExecutable(): Promise<string | null> {
    return Promise.resolve("C:\\Windows\\System32\\notepad.exe");
  },

  loadInstalledFontFamilies(): Promise<ReadonlyArray<string>> {
    return Promise.resolve(["Segoe UI", "Arial", "Calibri", "Cambria", "Consolas", "맑은 고딕", "Microsoft YaHei UI", "Microsoft JhengHei UI", "Meiryo", "Tahoma"]);
  },

  setSessionAutostart(enabled: boolean): Promise<boolean> {
    return Promise.resolve(enabled);
  },

  launchTargetWithMactype(): Promise<number> {
    return Promise.resolve(4242);
  },

  scanInstallation(): Promise<InstallationStatus | null> {
    return Promise.resolve(null);
  },

  applyOpenProfile(): Promise<AppliedProfile> {
    return Promise.resolve<AppliedProfile>({ sourceProfile: fallbackProfile.path, runtimeRoot: "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\runtime\\generations\\gallery" });
  },

  registerSessionTarget(target: string, arguments_: ReadonlyArray<string>): Promise<ReadonlyArray<SessionTarget>> {
    return Promise.resolve<ReadonlyArray<SessionTarget>>([{ target, arguments: arguments_ }]);
  },

  removeSessionTarget(): Promise<ReadonlyArray<SessionTarget>> {
    return Promise.resolve([]);
  },

  launchRegisteredTargets(): Promise<ReadonlyArray<number>> {
    return Promise.resolve([4242]);
  },

  rediscoverInstallation(): Promise<InstallationStatus> {
    return Promise.resolve<InstallationStatus>({ ...fallbackStatus, state: "ready" });
  },

  reconnectPreview(): Promise<InstallationStatus> {
    return Promise.resolve<InstallationStatus>({
      ...fallbackStatus,
      state: "ready",
      findings: fallbackStatus.findings.map((finding) => finding.label === "preview" ? { ...finding, value: "connected", ok: true } : finding),
    });
  },

  loadDiagnosticReport(): Promise<string> {
    return Promise.resolve(`MacType Control Center diagnostics\ncontrolCenterVersion=0.1.0\ncoreVersion=${fallbackStatus.coreVersion}\n`);
  },

  exportDiagnostics(): Promise<string> {
    return Promise.resolve("C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\logs\\diagnostics-gallery.txt");
  },

  copyDiagnostics(): Promise<void> {
    return Promise.resolve();
  },

  openLogFolder(): Promise<string> {
    return Promise.resolve("C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\logs");
  },

  openDefaultProfile(): Promise<ProfileSnapshot | null> {
    return Promise.resolve(fallbackProfile);
  },

  listProfiles(): Promise<ReadonlyArray<ProfileEntry>> {
    return Promise.resolve([{ name: "Default", path: fallbackProfile.path }]);
  },

  openProfile(path: string): Promise<ProfileSnapshot> {
    return Promise.resolve({ ...fallbackProfile, path });
  },

  duplicateProfile(name: string): Promise<ProfileSnapshot> {
    return Promise.resolve({ ...fallbackProfile, path: `C:\\Program Files\\MacType\\ini\\${name}.ini` });
  },

  updateProfileSetting(): Promise<ProfileSnapshot | null> {
    return Promise.resolve(null);
  },

  updateProfileIndividuals(): Promise<ProfileSnapshot | null> {
    return Promise.resolve(null);
  },

  updateProfileList(): Promise<ProfileSnapshot | null> {
    return Promise.resolve(null);
  },

  updateProfileAdvanced(): Promise<ProfileSnapshot | null> {
    return Promise.resolve(null);
  },

  saveProfile(): Promise<ProfileSnapshot | null> {
    return Promise.resolve(null);
  },

  renderProfilePreview(): Promise<null> {
    return Promise.resolve(null);
  },

  setNativePreview(visible: boolean): Promise<boolean> {
    return Promise.resolve(visible);
  },

  previewImageUrl(path: string): string {
    return path;
  },

  loadPreviewDiagnostics(): Promise<ReadonlyArray<string>> {
    return Promise.resolve([]);
  },

  forcePreviewCrashForCi: () => Promise.resolve(),
  verifyProfileWorkflowForCi: () => Promise.resolve(),
  verifyInjectionWorkflowForCi: () => Promise.resolve(),
  verifyTrayModeForCi: () => Promise.resolve(),
  reportFrontendReady: () => Promise.resolve(),
  reportFrontendFailure: () => Promise.resolve(),
};
