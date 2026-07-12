export type ViewId = "overview" | "profiles" | "diagnostics";

export interface LaunchContext {
  view: ViewId;
  ciSmoke: boolean;
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

export interface ProfileSnapshot {
  path: string;
  encoding: string;
  bom: string;
  lineEnding: string;
  originalHash: string;
  values: Record<string, number>;
  dirtyKeys: ReadonlyArray<string>;
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

export const navigation: ReadonlyArray<{ id: ViewId; label: string; description: string }> = [
  { id: "overview", label: "개요", description: "설치와 적용 상태" },
  { id: "profiles", label: "프로필", description: "렌더링 설정 편집" },
  { id: "diagnostics", label: "진단", description: "구성 요소와 로그" },
];

export const fallbackStatus: InstallationStatus = {
  state: "incomplete",
  root: "C:\\Program Files\\MacType",
  coreVersion: "1.2025.6.9",
  findings: [
    { label: "32비트 코어", value: "MacType.dll", ok: true },
    { label: "64비트 코어", value: "MacType64.dll", ok: true },
    { label: "프리뷰 연결", value: "대기 중", ok: false },
  ],
};
