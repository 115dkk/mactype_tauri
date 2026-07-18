import type { ExecutionStatus, LegacyMacTrayStatus, LegacyTrayStatus } from "./model";
import type { MessageKey } from "../i18n/i18n";

type SystemInjectionState = "loading" | "active" | "running-unverified" | "running-profile-mismatch" | "running-appinit-conflict" | "running-legacy-tray-conflict" | "legacy-service-migrate" | "inactive" | "unavailable";

export type LegacyTrayResolutionKind =
  | "exit-current-process"
  | "other-session"
  | "untrusted-process"
  | "unknown-process"
  | "disable-autostart"
  | "untrusted-autostart"
  | "unknown-autostart";

export interface LegacyTrayResolution {
  kind: LegacyTrayResolutionKind;
  titleKey: MessageKey;
  descriptionKey: MessageKey;
  canRequestExit: boolean;
  canDisableStartup: boolean;
}

export interface SystemInjectionPrimaryAction {
  intent: "activate" | "stop";
  command: "publish-profile" | "stop";
  enabled: boolean;
  state: SystemInjectionState;
  titleKey: MessageKey;
  descriptionKey: MessageKey;
  labelKey: MessageKey;
}

export interface ExecutionViewModel {
  status: ExecutionStatus | null;
  profileMatches: boolean;
  serviceNeedsUpgrade: boolean;
  serviceNeedsRepair: boolean;
  serviceBinaryPath: string | null;
  systemInjectionAction: SystemInjectionPrimaryAction;
  legacyTrayResolution: LegacyTrayResolution | null;
  canInstall: boolean;
  canStart: boolean;
  canUpgrade: boolean;
  canRepair: boolean;
  canRemove: boolean;
  canMigrateLegacy: boolean;
  canRemoveLegacy: boolean;
}

const systemInjectionCopy: Record<SystemInjectionState, { titleKey: MessageKey; descriptionKey: MessageKey }> = {
  loading: {
    titleKey: "execution.checking",
    descriptionKey: "execution.checking",
  },
  active: {
    titleKey: "execution.systemActiveTitle",
    descriptionKey: "execution.systemActiveDescription",
  },
  "running-unverified": {
    titleKey: "execution.systemRunningUnverifiedTitle",
    descriptionKey: "execution.systemRunningUnverifiedDescription",
  },
  "running-profile-mismatch": {
    titleKey: "execution.systemProfileMismatchTitle",
    descriptionKey: "execution.systemProfileMismatchDescription",
  },
  "running-appinit-conflict": {
    titleKey: "execution.systemAppInitConflictTitle",
    descriptionKey: "execution.systemAppInitConflictDescription",
  },
  "running-legacy-tray-conflict": {
    titleKey: "execution.systemLegacyTrayConflictTitle",
    descriptionKey: "execution.systemLegacyTrayConflictDescription",
  },
  "legacy-service-migrate": {
    titleKey: "execution.systemLegacyServiceMigrateTitle",
    descriptionKey: "execution.systemLegacyServiceMigrateDescription",
  },
  inactive: {
    titleKey: "execution.systemInactiveTitle",
    descriptionKey: "execution.systemInactiveDescription",
  },
  unavailable: {
    titleKey: "execution.systemUnavailableTitle",
    descriptionKey: "execution.systemUnavailableDescription",
  },
};

function projectLegacyTrayResolution(status: LegacyTrayStatus | undefined): LegacyTrayResolution | null {
  if (!status || status.conflict === "clear") return null;

  switch (status.process.state) {
    case "trusted-current-session":
      return {
        kind: "exit-current-process",
        titleKey: "execution.legacyTrayRunningTitle",
        descriptionKey: "execution.legacyTrayRunningDescription",
        canRequestExit: status.canRequestExit,
        canDisableStartup: false,
      };
    case "trusted-other-session":
      return {
        kind: "other-session",
        titleKey: "execution.legacyTrayOtherSessionTitle",
        descriptionKey: "execution.legacyTrayOtherSessionDescription",
        canRequestExit: false,
        canDisableStartup: false,
      };
    case "untrusted-same-name":
      return {
        kind: "untrusted-process",
        titleKey: "execution.legacyTrayUntrustedTitle",
        descriptionKey: "execution.legacyTrayUntrustedDescription",
        canRequestExit: false,
        canDisableStartup: false,
      };
    case "unknown":
      return {
        kind: "unknown-process",
        titleKey: "execution.legacyTrayUnknownTitle",
        descriptionKey: "execution.legacyTrayUnknownDescription",
        canRequestExit: false,
        canDisableStartup: false,
      };
    case "absent":
      break;
  }

  switch (status.startup.state) {
    case "detected":
      return {
        kind: "disable-autostart",
        titleKey: "execution.legacyTrayAutostartTitle",
        descriptionKey: "execution.legacyTrayAutostartDescription",
        canRequestExit: false,
        canDisableStartup: status.canDisableStartup,
      };
    case "untrusted":
      return {
        kind: "untrusted-autostart",
        titleKey: "execution.legacyTrayAutostartUntrustedTitle",
        descriptionKey: "execution.legacyTrayAutostartUntrustedDescription",
        canRequestExit: false,
        canDisableStartup: false,
      };
    case "unknown":
      return {
        kind: "unknown-autostart",
        titleKey: "execution.legacyTrayAutostartUnknownTitle",
        descriptionKey: "execution.legacyTrayAutostartUnknownDescription",
        canRequestExit: false,
        canDisableStartup: false,
      };
    case "absent":
      return null;
  }
}

// A verified-shape legacy MacType service must be retired through the explicit
// Migrate transaction (which stops it before starting the new service); generic
// install/start/activate paths never arbitrate against it and could leave both
// services injecting at once.
function verifiedLegacyServicePresent(legacy: LegacyMacTrayStatus | null | undefined): boolean {
  return legacy?.presence === "owned" || legacy?.presence === "compatible-unquoted";
}

function projectSystemInjectionAction(
  status: ExecutionStatus | null,
  serviceBusy: string | null,
  profileMatches: boolean,
): SystemInjectionPrimaryAction {
  const service = status?.systemService;
  const running = service?.runtime === "running";
  const profileMismatch = Boolean(
    status?.expectedProfileDigest
      && service?.activeProfileDigest
      && service.activeProfileDigest !== status.expectedProfileDigest,
  );
  const legacyTrayConflict = Boolean(status && status.legacyTray.conflict !== "clear");
  const legacyServicePresent = verifiedLegacyServicePresent(status?.legacyMacTray);
  const legacyServiceContends = legacyServicePresent && status?.legacyMacTray?.state !== "stopped";
  const verifiedActive = Boolean(
    status?.systemInjectionActive
      && running
      && service?.health === "ready"
      && profileMatches
      && !status.registryModeDetected
      && !legacyTrayConflict
      && !legacyServiceContends,
  );

  let state: SystemInjectionState;
  if (!status) state = "loading";
  else if (running && legacyTrayConflict) state = "running-legacy-tray-conflict";
  else if (running && status.registryModeDetected) state = "running-appinit-conflict";
  else if (verifiedActive) state = "active";
  else if (running && profileMismatch) state = "running-profile-mismatch";
  else if (running) state = "running-unverified";
  else if (legacyServicePresent) state = "legacy-service-migrate";
  else if (service?.runtime === "stopped") state = "inactive";
  else state = "unavailable";

  const intent = running ? "stop" : "activate";
  const idle = serviceBusy === null;
  const enabled = Boolean(
    idle
      && status
      && (intent === "stop"
        ? service?.canStop
        : service?.runtime === "stopped"
          && status.systemModesSupported
          && status.legacyTray.conflict === "clear"
          && !legacyServicePresent),
  );

  return {
    intent,
    command: intent === "stop" ? "stop" : "publish-profile",
    enabled,
    state,
    ...systemInjectionCopy[state],
    labelKey: serviceBusy
      ? "execution.serviceWorking"
      : intent === "stop"
        ? "execution.systemPause"
        : "execution.systemApply",
  };
}

export function projectExecutionView(
  status: ExecutionStatus | null,
  serviceBusy: string | null,
): ExecutionViewModel {
  const service = status?.systemService;
  const legacy = status?.legacyMacTray;
  const idle = serviceBusy === null;
  const legacyTrayConflict = Boolean(status && status.legacyTray.conflict !== "clear");
  const legacyServicePresent = verifiedLegacyServicePresent(legacy);
  const profileMatches = Boolean(
    status?.expectedProfileDigest
      && service?.activeProfileDigest === status.expectedProfileDigest,
  );

  return {
    status,
    profileMatches,
    serviceNeedsUpgrade: service?.installation === "outdated",
    serviceNeedsRepair: service?.installation === "current"
      && (service.health === "degraded" || service.health === "failed"),
    serviceBinaryPath: service?.installation === "absent" ? null : service?.binaryPath ?? null,
    systemInjectionAction: projectSystemInjectionAction(status, serviceBusy, profileMatches),
    legacyTrayResolution: projectLegacyTrayResolution(status?.legacyTray),
    canInstall: Boolean(idle && !legacyTrayConflict && !legacyServicePresent && service?.canInstall),
    canStart: Boolean(idle && !legacyTrayConflict && !legacyServicePresent && service?.canStart),
    canUpgrade: Boolean(idle && !legacyTrayConflict && service?.canUpgrade),
    canRepair: Boolean(idle && !legacyTrayConflict && service?.canRepair),
    canRemove: Boolean(idle && !legacyTrayConflict && service?.canRemove),
    canMigrateLegacy: Boolean(idle && !legacyTrayConflict && legacy?.migrationAvailable),
    canRemoveLegacy: Boolean(idle && !legacyTrayConflict && legacy?.canRemove),
  };
}
