import type { Locale } from "../i18n/i18n";
import type {
  AdvancedProfile,
  AppliedProfile,
  ExecutionStatus,
  ExpectedLegacyTrayIdentity,
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
} from "./model";
import { browserGalleryAdapter } from "./runtimeAdapters/browserGalleryAdapter";
import { tauriRuntimeAdapter } from "./runtimeAdapters/tauriRuntimeAdapter";

export interface ControlCenterRuntimeAdapter {
  loadLaunchContext(): Promise<LaunchContext>;
  setApplicationLocale(locale: Locale): Promise<void>;
  loadExecutionStatus(): Promise<ExecutionStatus>;
  requestLegacyTrayExit(expectedIdentity: ExpectedLegacyTrayIdentity): Promise<ExecutionStatus>;
  disableLegacyTrayAutostart(): Promise<ExecutionStatus>;
  manageSystemService(action: SystemServiceAction): Promise<ExecutionStatus>;
  revealSystemService(): Promise<void>;
  pickExecutable(filterName: string): Promise<string | null>;
  pickIniProfile(filterName: string): Promise<string | null>;
  pickIniExportPath(filterName: string, defaultName: string): Promise<string | null>;
  loadInstalledFontFamilies(): Promise<ReadonlyArray<string>>;
  setSessionAutostart(enabled: boolean): Promise<boolean>;
  launchTargetWithMactype(target: string, arguments_: ReadonlyArray<string>): Promise<number>;
  scanInstallation(): Promise<InstallationStatus | null>;
  applyOpenProfile(): Promise<AppliedProfile>;
  registerSessionTarget(target: string, arguments_: ReadonlyArray<string>): Promise<ReadonlyArray<SessionTarget>>;
  removeSessionTarget(target: string): Promise<ReadonlyArray<SessionTarget>>;
  launchRegisteredTargets(): Promise<ReadonlyArray<number>>;
  rediscoverInstallation(): Promise<InstallationStatus>;
  reconnectPreview(): Promise<InstallationStatus>;
  loadDiagnosticReport(): Promise<string>;
  exportDiagnostics(): Promise<string>;
  copyDiagnostics(): Promise<void>;
  openLogFolder(): Promise<string>;
  openDefaultProfile(): Promise<ProfileSnapshot | null>;
  currentProfile(): Promise<ProfileSnapshot | null>;
  discoverLegacyProfile(): Promise<LegacyProfileCandidate | null>;
  importProfile(path: string): Promise<ProfileSnapshot>;
  listProfiles(): Promise<ReadonlyArray<ProfileEntry>>;
  openProfile(path: string): Promise<ProfileSnapshot>;
  duplicateProfile(name: string): Promise<ProfileSnapshot>;
  updateProfileSetting(settingId: string, value: number): Promise<ProfileSnapshot | null>;
  updateProfileIndividuals(entries: ReadonlyArray<IndividualSetting>): Promise<ProfileSnapshot | null>;
  updateProfileList(kind: string, entries: ReadonlyArray<string>): Promise<ProfileSnapshot | null>;
  updateProfileAdvanced(advanced: AdvancedProfile): Promise<ProfileSnapshot | null>;
  undoProfile(): Promise<ProfileSnapshot>;
  redoProfile(): Promise<ProfileSnapshot>;
  discardProfileChanges(): Promise<ProfileSnapshot>;
  exportProfile(path: string): Promise<string>;
  revealProfileFile(): Promise<string>;
  saveProfile(): Promise<ProfileSnapshot | null>;
  renderProfilePreview(request: PreviewRequest): Promise<PreviewResult | null>;
  setNativePreview(visible: boolean): Promise<boolean>;
  previewImageUrl(path: string): string;
  loadPreviewDiagnostics(): Promise<ReadonlyArray<string>>;
  forcePreviewCrashForCi(): Promise<void>;
  verifyProfileWorkflowForCi(): Promise<void>;
  verifyInjectionWorkflowForCi(): Promise<void>;
  verifyTrayModeForCi(): Promise<void>;
  reportFrontendReady(view: ViewId): Promise<void>;
  reportFrontendFailure(view: ViewId, message: string): Promise<void>;
}

export function getRuntimeAdapter(): ControlCenterRuntimeAdapter {
  return "__TAURI_INTERNALS__" in window ? tauriRuntimeAdapter : browserGalleryAdapter;
}
