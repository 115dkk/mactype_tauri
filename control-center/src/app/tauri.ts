import type { Locale } from "../i18n/i18n";
import type {
  AdvancedProfile,
  AppliedProfile,
  ExecutionStatus,
  IndividualSetting,
  InstallationStatus,
  LegacyProfileCandidate,
  LegacyServiceStatus,
  LaunchContext,
  PreviewRequest,
  PreviewResult,
  ProfileEntry,
  ProfileSnapshot,
  SessionTarget,
  ViewId,
} from "./model";
import { getRuntimeAdapter } from "./runtimeAdapter";

export async function loadLaunchContext(): Promise<LaunchContext> {
  return getRuntimeAdapter().loadLaunchContext();
}

export async function setApplicationLocale(locale: Locale): Promise<void> {
  return getRuntimeAdapter().setApplicationLocale(locale);
}

export async function loadExecutionStatus(): Promise<ExecutionStatus> {
  return getRuntimeAdapter().loadExecutionStatus();
}

export async function pickExecutable(filterName: string): Promise<string | null> {
  return getRuntimeAdapter().pickExecutable(filterName);
}

export async function manageLegacyService(action: "install" | "remove" | "start" | "stop"): Promise<LegacyServiceStatus> {
  return getRuntimeAdapter().manageLegacyService(action);
}

export async function pickIniProfile(filterName: string): Promise<string | null> {
  return getRuntimeAdapter().pickIniProfile(filterName);
}

export async function loadInstalledFontFamilies(): Promise<ReadonlyArray<string>> {
  return getRuntimeAdapter().loadInstalledFontFamilies();
}

export async function setSessionAutostart(enabled: boolean): Promise<boolean> {
  return getRuntimeAdapter().setSessionAutostart(enabled);
}

export async function launchTargetWithMactype(target: string, arguments_: ReadonlyArray<string>): Promise<number> {
  return getRuntimeAdapter().launchTargetWithMactype(target, arguments_);
}

export async function scanInstallation(): Promise<InstallationStatus | null> {
  return getRuntimeAdapter().scanInstallation();
}

export async function applyOpenProfile(): Promise<AppliedProfile> {
  return getRuntimeAdapter().applyOpenProfile();
}

export async function registerSessionTarget(target: string, arguments_: ReadonlyArray<string>): Promise<ReadonlyArray<SessionTarget>> {
  return getRuntimeAdapter().registerSessionTarget(target, arguments_);
}

export async function removeSessionTarget(target: string): Promise<ReadonlyArray<SessionTarget>> {
  return getRuntimeAdapter().removeSessionTarget(target);
}

export async function launchRegisteredTargets(): Promise<ReadonlyArray<number>> {
  return getRuntimeAdapter().launchRegisteredTargets();
}

export async function rediscoverInstallation(): Promise<InstallationStatus> {
  return getRuntimeAdapter().rediscoverInstallation();
}

export async function reconnectPreview(): Promise<InstallationStatus> {
  return getRuntimeAdapter().reconnectPreview();
}

export async function loadDiagnosticReport(): Promise<string> {
  return getRuntimeAdapter().loadDiagnosticReport();
}

export async function exportDiagnostics(): Promise<string> {
  return getRuntimeAdapter().exportDiagnostics();
}

export async function copyDiagnostics(): Promise<void> {
  return getRuntimeAdapter().copyDiagnostics();
}

export async function openLogFolder(): Promise<string> {
  return getRuntimeAdapter().openLogFolder();
}

export async function openDefaultProfile(): Promise<ProfileSnapshot | null> {
  return getRuntimeAdapter().openDefaultProfile();
}

export async function currentProfile(): Promise<ProfileSnapshot | null> {
  return getRuntimeAdapter().currentProfile();
}

export async function discoverLegacyProfile(): Promise<LegacyProfileCandidate | null> {
  return getRuntimeAdapter().discoverLegacyProfile();
}

export async function importProfile(path: string): Promise<ProfileSnapshot> {
  return getRuntimeAdapter().importProfile(path);
}

export async function listProfiles(): Promise<ReadonlyArray<ProfileEntry>> {
  return getRuntimeAdapter().listProfiles();
}

export async function openProfile(path: string): Promise<ProfileSnapshot> {
  return getRuntimeAdapter().openProfile(path);
}

export async function duplicateProfile(name: string): Promise<ProfileSnapshot> {
  return getRuntimeAdapter().duplicateProfile(name);
}

export async function updateProfileSetting(settingId: string, value: number): Promise<ProfileSnapshot | null> {
  return getRuntimeAdapter().updateProfileSetting(settingId, value);
}

export async function updateProfileIndividuals(entries: ReadonlyArray<IndividualSetting>): Promise<ProfileSnapshot | null> {
  return getRuntimeAdapter().updateProfileIndividuals(entries);
}

export async function updateProfileList(kind: string, entries: ReadonlyArray<string>): Promise<ProfileSnapshot | null> {
  return getRuntimeAdapter().updateProfileList(kind, entries);
}

export async function updateProfileAdvanced(advanced: AdvancedProfile): Promise<ProfileSnapshot | null> {
  return getRuntimeAdapter().updateProfileAdvanced(advanced);
}

export async function saveProfile(): Promise<ProfileSnapshot | null> {
  return getRuntimeAdapter().saveProfile();
}

export async function renderProfilePreview(request: PreviewRequest): Promise<PreviewResult | null> {
  return getRuntimeAdapter().renderProfilePreview(request);
}

export async function setNativePreview(visible: boolean): Promise<boolean> {
  return getRuntimeAdapter().setNativePreview(visible);
}

export function previewImageUrl(path: string): string {
  return getRuntimeAdapter().previewImageUrl(path);
}

export async function loadPreviewDiagnostics(): Promise<ReadonlyArray<string>> {
  return getRuntimeAdapter().loadPreviewDiagnostics();
}

export async function forcePreviewCrashForCi(): Promise<void> {
  return getRuntimeAdapter().forcePreviewCrashForCi();
}

export async function verifyProfileWorkflowForCi(): Promise<void> {
  return getRuntimeAdapter().verifyProfileWorkflowForCi();
}

export async function verifyInjectionWorkflowForCi(): Promise<void> {
  return getRuntimeAdapter().verifyInjectionWorkflowForCi();
}

export async function verifyTrayModeForCi(): Promise<void> {
  return getRuntimeAdapter().verifyTrayModeForCi();
}

export async function reportFrontendReady(view: ViewId): Promise<void> {
  return getRuntimeAdapter().reportFrontendReady(view);
}

export async function reportFrontendFailure(view: ViewId, message: string): Promise<void> {
  return getRuntimeAdapter().reportFrontendFailure(view, message);
}
