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
  legacyService: LegacyServiceStatus;
  registryModeDetected: boolean;
  systemModesSupported: boolean;
  systemInjectionActive: boolean;
  systemModeNote: string;
  injectionReady: boolean;
  activeProfile: string | null;
  sessionTargets: ReadonlyArray<SessionTarget>;
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

export interface LegacyServiceStatus {
  presence: "absent" | "owned" | "compatible-unquoted" | "foreign" | "delete-pending" | "inaccessible";
  state: "stopped" | "start-pending" | "stop-pending" | "running" | "continue-pending" | "pause-pending" | "paused" | "unknown";
  binaryPath: string | null;
  win32Error: number | null;
  trustedBinaryAvailable: boolean;
  registryConflict: boolean;
  canInstall: boolean;
  canRemove: boolean;
  canStart: boolean;
  canStop: boolean;
}

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
