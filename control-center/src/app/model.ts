export type ViewId = "overview" | "profiles" | "execution" | "diagnostics";

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
  legacyServiceDetected: boolean;
  legacyServiceRunning: boolean;
  registryModeDetected: boolean;
  systemModesSupported: boolean;
  systemModeNote: string;
}

export interface ProfileSnapshot {
  path: string;
  encoding: string;
  bom: string;
  lineEnding: string;
  originalHash: string;
  values: Record<string, number>;
  dirtyKeys: ReadonlyArray<string>;
  individuals: ReadonlyArray<IndividualSetting>;
  lists: ProfileLists;
}

export interface ProfileEntry {
  name: string;
  path: string;
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
