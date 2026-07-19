export type ViewId = "overview" | "files" | "profiles" | "execution" | "diagnostics";

export interface LaunchContext {
  view: ViewId;
  ciSmoke: boolean;
  trayStart: boolean;
}

export interface InstallationStatus {
  state: "ready" | "incomplete" | "not-found";
  root: string | null;
  coreVersion: string | null;
  findings: ReadonlyArray<{ label: string; value: string; ok: boolean }>;
}

export interface DiagnosticEntry {
  time: string;
  area: string;
  message: string;
  severity: "info" | "warning" | "error";
}

export interface ExecutionStatus {
  trayAvailable: boolean;
  autoStart: boolean;
  manualLauncherAvailable: boolean;
  systemService: SystemServiceStatus;
  legacyMacTray: LegacyMacTrayStatus | null;
  legacyTray: LegacyTrayStatus;
  registryModeDetected: boolean;
  /** Backend-authoritative capability for publishing and applying a profile. */
  systemModesSupported: boolean;
  systemInjectionActive: boolean;
  injectionReady: boolean;
  activeProfile: string | null;
  expectedProfileDigest: string | null;
  sessionTargets: ReadonlyArray<SessionTarget>;
}

export interface StructuredServiceError {
  code: string;
  message: string;
  win32_error: number | null;
}

export type LegacyTrayProcessState =
  | { state: "absent" }
  | { state: "trusted-current-session"; pid: number; creationTime: string; path: string }
  | { state: "trusted-other-session"; sessionId: number; path: string }
  | { state: "untrusted-same-name"; sessionId: number | null; path: string | null }
  | { state: "unknown"; error: StructuredServiceError };

export type LegacyTrayStartupSource =
  | "current-user-run32"
  | "current-user-run64"
  | "local-machine-run32"
  | "local-machine-run64"
  | "current-user-startup";

export interface LegacyTrayStartupEntry {
  sourceKind: LegacyTrayStartupSource;
  displayName: string;
  targetPath: string;
}

export type LegacyTrayStartupState =
  | { state: "absent" }
  | { state: "detected"; entries: ReadonlyArray<LegacyTrayStartupEntry> }
  | { state: "untrusted"; entries: ReadonlyArray<LegacyTrayStartupEntry> }
  | { state: "unknown"; error: StructuredServiceError };

export type LegacyTrayConflictState = "clear" | "detected" | "unknown";

export interface LegacyTrayStatus {
  process: LegacyTrayProcessState;
  startup: LegacyTrayStartupState;
  conflict: LegacyTrayConflictState;
  canRequestExit: boolean;
  canDisableStartup: boolean;
}

export interface ExpectedLegacyTrayIdentity {
  pid: number;
  creationTime: string;
  path: string;
}

export interface SessionTarget {
  target: string;
  arguments: ReadonlyArray<string>;
}

export interface AppliedProfile {
  sourceProfile: string;
  runtimeRoot: string;
}

export interface ProfileSnapshot {
  path: string;
  encoding: string;
  bom: string;
  lineEnding: string;
  originalHash: string;
  values: Record<string, number>;
  dirtyKeys: ReadonlyArray<string>;
  canUndo: boolean;
  canRedo: boolean;
  individuals: ReadonlyArray<IndividualSetting>;
  lists: ProfileLists;
  advanced: AdvancedProfile;
}

export interface ProfileEntry {
  name: string;
  path: string;
}

export type ServiceBackend = "open-source" | "legacy-mac-tray" | "foreign" | "none";
export type InstallationState = "absent" | "current" | "outdated" | "invalid" | "inaccessible" | "delete-pending";
export type ServiceRuntimeState = "stopped" | "start-pending" | "running" | "stop-pending" | "paused" | "unknown";
export type ServiceHealthState = "unknown" | "initializing" | "ready" | "degraded" | "failed";

export interface SystemServiceStatus {
  backend: ServiceBackend;
  installation: InstallationState;
  runtime: ServiceRuntimeState;
  health: ServiceHealthState;
  binaryPath: string | null;
  win32Error: number | null;
  activeProfileDigest: string | null;
  canInstall: boolean;
  canRemove: boolean;
  canStart: boolean;
  canStop: boolean;
  canRepair: boolean;
  canUpgrade: boolean;
}

export interface LegacyMacTrayStatus {
  presence: "absent" | "owned" | "compatible-unquoted" | "foreign" | "delete-pending" | "inaccessible";
  state: ServiceRuntimeState | "continue-pending" | "pause-pending";
  binaryPath: string | null;
  win32Error: number | null;
  trustedBinaryAvailable: boolean;
  registryConflict: boolean;
  canRemove: boolean;
  canStop: boolean;
  migrationAvailable: boolean;
  migrationBackupAvailable: boolean;
  blocksActivation: boolean;
}

export type SystemServiceAction = "install" | "upgrade" | "repair" | "remove" | "start" | "stop" | "publish-profile" | "migrate-from-legacy" | "remove-legacy";

export interface LegacyProfileCandidate {
  name: string;
  path: string;
  source: "alternative-file";
}

export interface IndividualSetting {
  fontFace: string;
  values: Array<number | null>;
}

export interface ProfileLists {
  excludeFonts: ReadonlyArray<string>;
  includeFonts: ReadonlyArray<string>;
  excludeModules: ReadonlyArray<string>;
  includeModules: ReadonlyArray<string>;
  unloadDlls: ReadonlyArray<string>;
  excludeSubstitutionModules: ReadonlyArray<string>;
}

export interface ShadowSetting {
  offsetX: number;
  offsetY: number;
  darkAlpha: number;
  darkColor: number;
  lightAlpha: number;
  lightColor: number;
}

export interface AdvancedProfile {
  shadow: ShadowSetting | null;
  lcdFilterWeight: ReadonlyArray<number> | null;
  pixelLayout: ReadonlyArray<number> | null;
  displayAffinity: ReadonlyArray<number>;
  fontSubstitutes: ReadonlyArray<string>;
  infinalityGammaCorrection: ReadonlyArray<number>;
  infinalityFilterParams: ReadonlyArray<number>;
}

export interface PreviewSample {
  text: string;
  fontFace: string;
  fontSizePt: number;
  widthPx: number;
  heightPx: number;
  dpi: number;
  foreground: string;
  background: string;
}

export interface PreviewRequest {
  profilePath: string;
  overrides: Record<string, number>;
  sample: PreviewSample;
  displayScale: number;
}

export interface PreviewResult {
  requestId: number;
  imagePath: string;
  width: number;
  height: number;
  dpi: number;
  elapsedMs: number;
  coreVersion: number;
}

export const fallbackStatus: InstallationStatus = {
  state: "incomplete",
  root: "C:\\Program Files\\MacType",
  coreVersion: "1.2025.6.9",
  findings: [
    { label: "core32", value: "MacType.dll", ok: true },
    { label: "core64", value: "MacType64.dll", ok: true },
    { label: "preview", value: "waiting", ok: false },
  ],
};
