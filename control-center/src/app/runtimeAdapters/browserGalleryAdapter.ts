import { settingsSchema } from "../../generated/settings";
import {
  fallbackStatus,
  type AppliedProfile,
  type ExecutionStatus,
  type InstallationStatus,
  type LegacyProfileCandidate,
  type LegacyServiceStatus,
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
  canUndo: false,
  canRedo: false,
  individuals: [{ fontFace: "Segoe UI", values: [1, 2, null, null, null, 1] }],
  lists: { excludeFonts: ["Raster Fonts"], includeFonts: [], excludeModules: ["fontview.exe"], includeModules: [], unloadDlls: [], excludeSubstitutionModules: [] },
  advanced: { shadow: null, lcdFilterWeight: null, pixelLayout: null, displayAffinity: [], fontSubstitutes: [], infinalityGammaCorrection: [0, 100], infinalityFilterParams: [11, 22, 38, 22, 11] },
};
const recentGalleryProfile = { ...fallbackProfile, path: "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\profiles\\Recent.ini" };

const cloneProfile = (profile: ProfileSnapshot): ProfileSnapshot => structuredClone(profile);
let galleryProfile = cloneProfile(fallbackProfile);
let savedGalleryProfile = cloneProfile(fallbackProfile);
const galleryUndo: ProfileSnapshot[] = [];
const galleryRedo: ProfileSnapshot[] = [];

function openGalleryProfile(profile: ProfileSnapshot): ProfileSnapshot {
  galleryProfile = { ...cloneProfile(profile), canUndo: false, canRedo: false };
  savedGalleryProfile = cloneProfile(galleryProfile);
  galleryUndo.length = 0;
  galleryRedo.length = 0;
  return cloneProfile(galleryProfile);
}

function editGalleryProfile(update: (profile: ProfileSnapshot) => ProfileSnapshot, dirtyKey: string): ProfileSnapshot {
  galleryUndo.push(cloneProfile(galleryProfile));
  galleryRedo.length = 0;
  const next = update(cloneProfile(galleryProfile));
  galleryProfile = {
    ...next,
    dirtyKeys: [...new Set([...next.dirtyKeys, dirtyKey])],
    canUndo: true,
    canRedo: false,
  };
  return cloneProfile(galleryProfile);
}

function moveGalleryHistory(from: ProfileSnapshot[], to: ProfileSnapshot[]): ProfileSnapshot {
  const destination = from.pop();
  if (!destination) return cloneProfile(galleryProfile);
  to.push(cloneProfile(galleryProfile));
  galleryProfile = {
    ...cloneProfile(destination),
    canUndo: galleryUndo.length > 0,
    canRedo: galleryRedo.length > 0,
  };
  return cloneProfile(galleryProfile);
}

const galleryService: LegacyServiceStatus = {
  presence: "owned",
  state: "running",
  binaryPath: "C:\\Program Files\\MacType\\MacTray.exe -service",
  win32Error: null,
  trustedBinaryAvailable: true,
  registryConflict: false,
  canInstall: false,
  canRemove: true,
  canStart: false,
  canStop: true,
};

export const browserGalleryAdapter: ControlCenterRuntimeAdapter = {
  loadLaunchContext(): Promise<LaunchContext> {
    const requested = new URLSearchParams(window.location.search).get("view");
    return Promise.resolve<LaunchContext>({
      view: requested === "files" || requested === "profiles" || requested === "execution" || requested === "diagnostics" ? requested : "overview",
      ciSmoke: false,
      trayStart: false,
    });
  },

  setApplicationLocale(): Promise<void> {
    return Promise.resolve();
  },

  loadExecutionStatus(): Promise<ExecutionStatus> {
    const activeProfile = new URLSearchParams(window.location.search).has("legacy-applied")
      ? "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\profiles\\Pretendard forever.ini"
      : fallbackProfile.path;
    return Promise.resolve<ExecutionStatus>({ trayAvailable: true, autoStart: false, manualLauncherAvailable: true, legacyService: galleryService, registryModeDetected: false, systemModesSupported: true, systemInjectionActive: true, systemModeNote: "검증된 레거시 서비스로 현재 프로필을 시스템 범위에 적용합니다.", injectionReady: true, activeProfile, sessionTargets: [] });
  },

  manageLegacyService(action): Promise<LegacyServiceStatus> {
    const state = action === "stop" || action === "remove" ? "stopped" : "running";
    return Promise.resolve({
      ...galleryService,
      presence: action === "remove" ? "absent" : "owned",
      state,
      canInstall: action === "remove",
      canRemove: action !== "remove",
      canStart: action === "stop",
      canStop: action === "start" || action === "install",
    });
  },

  activateSystemInjection(): Promise<ExecutionStatus> {
    return this.loadExecutionStatus();
  },

  pickExecutable(): Promise<string | null> {
    return Promise.resolve("C:\\Windows\\System32\\notepad.exe");
  },

  pickIniProfile(): Promise<string | null> {
    return Promise.resolve("C:\\Users\\Gallery\\Downloads\\Community.ini");
  },

  pickIniExportPath(_filterName, defaultName): Promise<string | null> {
    return Promise.resolve(`C:\\Users\\Gallery\\Documents\\${defaultName}`);
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
    return Promise.resolve<AppliedProfile>({ sourceProfile: galleryProfile.path, runtimeRoot: "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\runtime\\generations\\gallery" });
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
    return Promise.resolve(openGalleryProfile(fallbackProfile));
  },

  currentProfile(): Promise<ProfileSnapshot | null> {
    if (new URLSearchParams(window.location.search).has("fresh")) return Promise.resolve(null);
    return Promise.resolve(cloneProfile(galleryProfile));
  },

  discoverLegacyProfile(): Promise<LegacyProfileCandidate | null> {
    return Promise.resolve({ name: "Pretendard forever", path: "C:\\Program Files\\MacType\\ini\\pretendard forever.ini", source: "alternative-file" });
  },

  importProfile(path: string): Promise<ProfileSnapshot> {
    return Promise.resolve(openGalleryProfile({ ...fallbackProfile, path: `C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\profiles\\${path.split(/[\\/]/).pop() ?? "Imported.ini"}` }));
  },

  listProfiles(): Promise<ReadonlyArray<ProfileEntry>> {
    return Promise.resolve([
      { name: "Default", path: fallbackProfile.path },
      { name: "Recent", path: recentGalleryProfile.path },
    ]);
  },

  openProfile(path: string): Promise<ProfileSnapshot> {
    return Promise.resolve(openGalleryProfile({ ...fallbackProfile, path }));
  },

  duplicateProfile(name: string): Promise<ProfileSnapshot> {
    return Promise.resolve(openGalleryProfile({ ...galleryProfile, path: `C:\\Program Files\\MacType\\ini\\${name}.ini` }));
  },

  updateProfileSetting(settingId, value): Promise<ProfileSnapshot | null> {
    return Promise.resolve(editGalleryProfile((profile) => ({ ...profile, values: { ...profile.values, [settingId]: value } }), settingId));
  },

  updateProfileIndividuals(entries): Promise<ProfileSnapshot | null> {
    return Promise.resolve(editGalleryProfile((profile) => ({ ...profile, individuals: structuredClone(entries) }), "section:Individual"));
  },

  updateProfileList(kind, entries): Promise<ProfileSnapshot | null> {
    return Promise.resolve(editGalleryProfile((profile) => ({ ...profile, lists: { ...profile.lists, [kind]: [...entries] } }), `section:${kind}`));
  },

  updateProfileAdvanced(advanced): Promise<ProfileSnapshot | null> {
    return Promise.resolve(editGalleryProfile((profile) => ({ ...profile, advanced: structuredClone(advanced) }), "advanced"));
  },

  undoProfile(): Promise<ProfileSnapshot> {
    return Promise.resolve(moveGalleryHistory(galleryUndo, galleryRedo));
  },

  redoProfile(): Promise<ProfileSnapshot> {
    return Promise.resolve(moveGalleryHistory(galleryRedo, galleryUndo));
  },

  discardProfileChanges(): Promise<ProfileSnapshot> {
    return Promise.resolve(openGalleryProfile(savedGalleryProfile));
  },

  exportProfile(path: string): Promise<string> {
    return Promise.resolve(path);
  },

  revealProfileFile(): Promise<string> {
    return Promise.resolve(galleryProfile.path);
  },

  saveProfile(): Promise<ProfileSnapshot | null> {
    galleryProfile = { ...galleryProfile, dirtyKeys: [], canUndo: false, canRedo: false };
    savedGalleryProfile = cloneProfile(galleryProfile);
    galleryUndo.length = 0;
    galleryRedo.length = 0;
    return Promise.resolve(cloneProfile(galleryProfile));
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
