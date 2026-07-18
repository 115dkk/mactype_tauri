import type {
  ExecutionStatus,
  LegacyMacTrayStatus,
  ServiceRuntimeState,
  SystemServiceAction,
  SystemServiceStatus,
} from "../model";
import { fallbackGalleryProfilePath } from "./browserGalleryProfiles";

export const expectedGalleryDigest = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

type GalleryQuery = Pick<URLSearchParams, "get" | "has">;

const galleryLegacyService: LegacyMacTrayStatus = {
  presence: "owned",
  state: "running",
  binaryPath: null,
  win32Error: null,
  trustedBinaryAvailable: true,
  registryConflict: false,
  canRemove: true,
  canStop: true,
  migrationAvailable: true,
  migrationBackupAvailable: false,
};

const serviceRuntimeValues: ReadonlyArray<ServiceRuntimeState> = [
  "stopped",
  "start-pending",
  "running",
  "stop-pending",
  "paused",
  "unknown",
];

const legacyRuntimeValues: ReadonlyArray<LegacyMacTrayStatus["state"]> = [
  "stopped",
  "start-pending",
  "running",
  "stop-pending",
  "continue-pending",
  "pause-pending",
  "paused",
  "unknown",
];

export function galleryExecutionStatus(query: GalleryQuery): ExecutionStatus {
  const fixture = query.get("system-service") ?? "ready";
  const appInitConflict = fixture === "legacy-conflict";
  const legacyTrayDetected = query.get("legacy-tray") === "running";
  const profileMismatch = fixture === "profile-mismatch";
  const ready = fixture === "ready" || appInitConflict;
  const requestedServiceRuntime = query.get("service-runtime") ?? fixture;
  const defaultServiceRuntime: ServiceRuntimeState = ready || fixture === "degraded" || profileMismatch
    ? "running"
    : "stopped";
  const serviceRuntime = serviceRuntimeValues.find((runtime) => runtime === requestedServiceRuntime)
    ?? defaultServiceRuntime;
  const serviceStable = serviceRuntime === "running" || serviceRuntime === "stopped";
  const serviceBackend = fixture === "foreign-service" ? "foreign" : "open-source";
  const serviceInstallation = fixture === "foreign-service"
    ? "invalid"
    : fixture === "migration-available"
      ? "absent"
      : fixture === "outdated"
        ? "outdated"
        : "current";
  const generalMutationAllowed = serviceBackend === "open-source" && serviceStable && !appInitConflict && !legacyTrayDetected;
  const systemModesSupported = generalMutationAllowed
    && (serviceInstallation === "absent" || serviceInstallation === "current" || serviceInstallation === "outdated");
  const activeProfile = query.has("legacy-applied")
    ? "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\profiles\\Pretendard forever.ini"
    : fallbackGalleryProfilePath;
  const legacyRequested = query.get("legacy") === "migration-available"
    || fixture === "legacy-conflict"
    || fixture === "migration-available";
  const requestedLegacyState = query.get("legacy-state");
  const legacyState = legacyRuntimeValues.find((state) => state === requestedLegacyState) ?? "running";
  const legacyStable = legacyState === "running" || legacyState === "stopped";

  return {
    trayAvailable: true,
    autoStart: false,
    manualLauncherAvailable: true,
    systemService: {
      backend: serviceBackend,
      installation: serviceInstallation,
      runtime: serviceRuntime,
      health: ready ? "ready" : fixture === "degraded" ? "degraded" : fixture === "failed" ? "failed" : "unknown",
      binaryPath: fixture === "migration-available"
        ? null
        : fixture === "foreign-service"
          ? "C:\\Program Files\\Unknown\\service.exe"
          : "C:\\Program Files\\MacType Control Center\\Service\\mactype-service.exe",
      win32Error: null,
      activeProfileDigest: profileMismatch
        ? "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        : ready
          ? expectedGalleryDigest
          : null,
      canInstall: generalMutationAllowed && serviceInstallation === "absent",
      canRemove: generalMutationAllowed && (serviceInstallation === "current" || serviceInstallation === "outdated"),
      canStart: generalMutationAllowed && serviceInstallation === "current" && serviceRuntime === "stopped",
      canStop: serviceBackend === "open-source" && serviceRuntime === "running",
      canRepair: generalMutationAllowed && serviceInstallation === "current",
      canUpgrade: generalMutationAllowed && serviceInstallation === "outdated",
    },
    legacyMacTray: legacyRequested ? {
      ...galleryLegacyService,
      state: legacyState,
      registryConflict: appInitConflict,
      canRemove: false,
      canStop: !appInitConflict && legacyState === "running",
      migrationAvailable: systemModesSupported && legacyStable,
    } : null,
    registryModeDetected: appInitConflict,
    legacyTrayDetected,
    systemModesSupported,
    systemInjectionActive: query.has("raw-active") ? true : ready && !appInitConflict,
    injectionReady: true,
    activeProfile,
    expectedProfileDigest: expectedGalleryDigest,
    sessionTargets: [],
  };
}

function runningGalleryService(current: SystemServiceStatus): SystemServiceStatus {
  return {
    ...current,
    backend: "open-source",
    installation: "current",
    runtime: "running",
    health: "ready",
    activeProfileDigest: expectedGalleryDigest,
    canInstall: false,
    canRemove: true,
    canStart: false,
    canStop: true,
    canRepair: false,
    canUpgrade: false,
  };
}

export function transitionGalleryExecutionStatus(
  current: ExecutionStatus,
  action: SystemServiceAction,
): ExecutionStatus {
  if (action === "stop") {
    return {
      ...current,
      systemInjectionActive: false,
      systemService: {
        ...current.systemService,
        runtime: "stopped",
        health: "unknown",
        activeProfileDigest: null,
        canStart: !current.registryModeDetected && current.systemService.installation === "current",
        canStop: false,
      },
    };
  }
  if (action === "remove") {
    return {
      ...current,
      systemInjectionActive: false,
      systemService: {
        ...current.systemService,
        installation: "absent",
        runtime: "stopped",
        health: "unknown",
        activeProfileDigest: null,
        canInstall: true,
        canRemove: false,
        canStart: false,
        canStop: false,
        canRepair: false,
        canUpgrade: false,
      },
    };
  }
  if (action === "remove-legacy") return { ...current, legacyMacTray: null };

  return {
    ...current,
    systemInjectionActive: true,
    systemService: runningGalleryService(current.systemService),
    legacyMacTray: action === "migrate-from-legacy" && current.legacyMacTray
      ? {
          ...current.legacyMacTray,
          state: "stopped",
          canStop: false,
          canRemove: true,
          migrationBackupAvailable: true,
        }
      : current.legacyMacTray,
  };
}
