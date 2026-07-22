import {
  fallbackStatus,
  type AppliedProfile,
  type ExecutionStatus,
  type InstallationStatus,
  type LegacyProfileCandidate,
  type LaunchContext,
  type ProfileEntry,
  type PreviewResult,
  type ProfileSnapshot,
  type RecentActivity,
  type SessionTarget,
} from "../model";
import type { ControlCenterRuntimeAdapter } from "../runtimeAdapter";
import {
  galleryExecutionStatus,
  transitionGalleryLegacyTrayAutostartDisable,
  transitionGalleryLegacyTrayExit,
  transitionGalleryExecutionStatus,
} from "./browserGalleryExecution";
import { createBrowserGalleryProfileState } from "./browserGalleryProfiles";

const galleryProfiles = createBrowserGalleryProfileState();
let galleryPreviewRequestId = 0;
let galleryExecutionState: { location: string; status: ExecutionStatus } | null = null;

function currentGalleryExecutionStatus(): ExecutionStatus {
  const location = window.location.href;
  if (!galleryExecutionState || galleryExecutionState.location !== location) {
    galleryExecutionState = {
      location,
      status: galleryExecutionStatus(new URLSearchParams(window.location.search)),
    };
  }
  return galleryExecutionState.status;
}

function updateGalleryExecutionStatus(status: ExecutionStatus): ExecutionStatus {
  galleryExecutionState = { location: window.location.href, status };
  return status;
}

function incrementGalleryCounter(key: string): void {
  const current = Number(window.sessionStorage.getItem(key) ?? "0");
  window.sessionStorage.setItem(key, String(current + 1));
}

export const browserGalleryAdapter: ControlCenterRuntimeAdapter = {
  loadLaunchContext(): Promise<LaunchContext> {
    const query = new URLSearchParams(window.location.search);
    const requested = query.get("view");
    return Promise.resolve<LaunchContext>({
      view: requested === "files" || requested === "profiles" || requested === "execution" || requested === "diagnostics" ? requested : "overview",
      ciSmoke: query.has("ci-smoke"),
      trayStart: false,
    });
  },

  setApplicationLocale(): Promise<void> {
    return Promise.resolve();
  },

  loadExecutionStatus(): Promise<ExecutionStatus> {
    return Promise.resolve(currentGalleryExecutionStatus());
  },

  requestLegacyTrayExit(expectedIdentity): Promise<ExecutionStatus> {
    return Promise.resolve(updateGalleryExecutionStatus(
      transitionGalleryLegacyTrayExit(currentGalleryExecutionStatus(), expectedIdentity),
    ));
  },

  disableLegacyTrayAutostart(): Promise<ExecutionStatus> {
    return Promise.resolve(updateGalleryExecutionStatus(
      transitionGalleryLegacyTrayAutostartDisable(currentGalleryExecutionStatus()),
    ));
  },

  manageSystemService(action): Promise<ExecutionStatus> {
    const query = new URLSearchParams(window.location.search);
    if (query.get("service-fail") === action) {
      return Promise.reject(new Error(`control-center-internal-operation-failed:${action}`));
    }
    const current = currentGalleryExecutionStatus();
    const next = transitionGalleryExecutionStatus(current, action);
    const delay = Number(query.get("service-delay"));
    if (Number.isFinite(delay) && delay > 0) {
      return new Promise((resolve) => window.setTimeout(() => resolve(updateGalleryExecutionStatus(next)), delay));
    }
    return Promise.resolve(updateGalleryExecutionStatus(next));
  },

  revealSystemService(): Promise<void> {
    return Promise.resolve();
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
    const query = new URLSearchParams(window.location.search);
    if (query.get("service-fail") === "publish-profile") {
      return Promise.reject(new Error("control-center-internal-operation-failed:publish-profile"));
    }
    updateGalleryExecutionStatus(
      transitionGalleryExecutionStatus(currentGalleryExecutionStatus(), "publish-profile"),
    );
    return Promise.resolve<AppliedProfile>({ sourceProfile: galleryProfiles.current().displayPath, runtimeRoot: "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\runtime\\generations\\gallery" });
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

  loadDiagnosticLogs(): Promise<ReadonlyArray<string>> {
    return Promise.resolve([
      "1784459527000 operation=migrate-from-legacy stage=verify open service readiness error=strict Ready timed out rollback=completed finalState=legacy=Running/Auto; modern=Absent",
    ]);
  },

  loadRecentActivity(): Promise<ReadonlyArray<RecentActivity>> {
    const now = Date.now();
    return Promise.resolve([
      { timestampUnixMs: now - 60_000, activity: "profile-verified", profile: "Default.ini" },
      { timestampUnixMs: now - 45_000, activity: "service-started", profile: null },
      { timestampUnixMs: now - 15_000, activity: "profile-applied", profile: "Default.ini" },
    ]);
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
    return Promise.resolve(galleryProfiles.openDefault());
  },

  currentProfile(): Promise<ProfileSnapshot | null> {
    if (new URLSearchParams(window.location.search).has("fresh")) return Promise.resolve(null);
    galleryProfiles.setCanSave(!new URLSearchParams(window.location.search).has("profile-read-only"));
    return Promise.resolve(galleryProfiles.current());
  },

  discoverLegacyProfile(): Promise<LegacyProfileCandidate | null> {
    if (new URLSearchParams(window.location.search).get("legacy-profile") === "external") {
      return Promise.resolve({ name: "External", path: "C:\\Users\\Gallery\\Downloads\\External.ini", source: "alternative-file" });
    }
    return Promise.resolve({ name: "Pretendard forever", path: "C:\\Program Files\\MacType\\ini\\pretendard forever.ini", source: "alternative-file" });
  },

  importProfile(path: string): Promise<ProfileSnapshot> {
    return Promise.resolve(galleryProfiles.import(path));
  },

  listProfiles(): Promise<ReadonlyArray<ProfileEntry>> {
    return Promise.resolve(galleryProfiles.list());
  },

  openProfile(path: string): Promise<ProfileSnapshot> {
    return Promise.resolve(galleryProfiles.open(path));
  },

  duplicateProfile(name: string): Promise<ProfileSnapshot> {
    return Promise.resolve(galleryProfiles.duplicate(name));
  },

  updateProfileSetting(settingId, value): Promise<ProfileSnapshot | null> {
    if (new URLSearchParams(window.location.search).get("profile-fail-setting") === settingId) {
      return Promise.reject(new Error("Gallery profile mutation failed."));
    }
    return Promise.resolve(galleryProfiles.updateSetting(settingId, value));
  },

  updateProfileIndividuals(entries): Promise<ProfileSnapshot | null> {
    return Promise.resolve(galleryProfiles.updateIndividuals(entries));
  },

  updateProfileList(kind, entries): Promise<ProfileSnapshot | null> {
    return Promise.resolve(galleryProfiles.updateList(kind, entries));
  },

  updateProfileAdvanced(advanced): Promise<ProfileSnapshot | null> {
    return Promise.resolve(galleryProfiles.updateAdvanced(advanced));
  },

  undoProfile(): Promise<ProfileSnapshot> {
    return Promise.resolve(galleryProfiles.undo());
  },

  redoProfile(): Promise<ProfileSnapshot> {
    return Promise.resolve(galleryProfiles.redo());
  },

  discardProfileChanges(): Promise<ProfileSnapshot> {
    return Promise.resolve(galleryProfiles.discard());
  },
  resetProfileDefaults(): Promise<ProfileSnapshot> {
    return Promise.resolve(galleryProfiles.resetDefaults());
  },

  exportProfile(path: string): Promise<string> {
    return Promise.resolve(path);
  },

  revealProfileFile(): Promise<string> {
    return Promise.resolve(galleryProfiles.current().path);
  },

  saveProfile(): Promise<ProfileSnapshot | null> {
    return Promise.resolve(galleryProfiles.save());
  },

  renderProfilePreview(request): Promise<PreviewResult | null> {
    const delay = Number(new URLSearchParams(window.location.search).get("preview-delay"));
    if (!Number.isFinite(delay) || delay <= 0) return Promise.resolve(null);
    incrementGalleryCounter("gallery-preview-started");
    const result: PreviewResult = {
      requestId: ++galleryPreviewRequestId,
      imagePath: "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==",
      width: request.sample.widthPx,
      height: request.sample.heightPx,
      dpi: request.sample.dpi,
      elapsedMs: delay,
      coreVersion: 0,
    };
    return new Promise((resolve) => window.setTimeout(() => resolve(result), delay));
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

  forcePreviewCrashForCi: () => {
    incrementGalleryCounter("gallery-preview-crashes");
    return Promise.resolve();
  },
  verifyProfileWorkflowForCi: () => Promise.resolve(),
  verifyInjectionWorkflowForCi: () => Promise.resolve(),
  verifyTrayModeForCi: () => Promise.resolve(),
  reportFrontendReady: (view) => {
    if (view === "profiles") incrementGalleryCounter("gallery-profile-ready");
    return Promise.resolve();
  },
  reportFrontendFailure: () => Promise.resolve(),
};
