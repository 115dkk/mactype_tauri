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
