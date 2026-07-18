import type { ExecutionStatus } from "./model";
import type { MessageKey } from "../i18n/i18n";

type SystemInjectionState = "loading" | "active" | "running-unverified" | "running-profile-mismatch" | "running-appinit-conflict" | "inactive" | "unavailable";

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
  inactive: {
    titleKey: "execution.systemInactiveTitle",
    descriptionKey: "execution.systemInactiveDescription",
  },
  unavailable: {
    titleKey: "execution.systemUnavailableTitle",
    descriptionKey: "execution.systemUnavailableDescription",
  },
};

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
  const verifiedActive = Boolean(
    status?.systemInjectionActive
      && running
      && service?.health === "ready"
      && profileMatches
      && !status.registryModeDetected,
  );

  let state: SystemInjectionState;
  if (!status) state = "loading";
  else if (running && status.registryModeDetected) state = "running-appinit-conflict";
  else if (verifiedActive) state = "active";
  else if (running && profileMismatch) state = "running-profile-mismatch";
  else if (running) state = "running-unverified";
  else if (service?.runtime === "stopped") state = "inactive";
  else state = "unavailable";

  const intent = running ? "stop" : "activate";
  const idle = serviceBusy === null;
  const enabled = Boolean(
    idle
      && status
      && (intent === "stop"
        ? service?.canStop
        : service?.runtime === "stopped" && status.systemModesSupported),
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
    canInstall: Boolean(idle && service?.canInstall),
    canStart: Boolean(idle && service?.canStart),
    canUpgrade: Boolean(idle && service?.canUpgrade),
    canRepair: Boolean(idle && service?.canRepair),
    canRemove: Boolean(idle && service?.canRemove),
    canMigrateLegacy: Boolean(idle && legacy?.migrationAvailable),
    canRemoveLegacy: Boolean(idle && legacy?.canRemove),
  };
}
