import type { ExecutionViewModel, ProfileIndicator, ServiceStatusLine, ServiceSummary, SystemInjectionPrimaryAction, LegacyTrayResolution, ServicePackageNotice } from "../../app/executionViewModel";
import type { SystemServiceStatus, LegacyMacTrayStatus } from "../../app/model";
import { useCallback, useEffect, useMemo, useRef, useState, type Dispatch, type SetStateAction, type RefObject, type KeyboardEvent as ReactKeyboardEvent } from "react";
import type { ExecutionStatus, ManualLaunchCandidate, SystemServiceAction } from "../../app/model";
import { projectExecutionView } from "../../app/executionViewModel";
import { operationErrorMessage } from "../../app/operationError";
import { runtime } from "../../app/runtimeAdapter";
import { useI18n } from "../../i18n/i18n";

export interface ExecutionModelOptions {
  ciSmoke?: boolean;
  onReady?: () => void;
}

/* The service page state machine, shared by every skin. A skin composes the
   rows, toolbars and dialogs differently, but the actions, busy flags,
   messages and the projected view come from this one hook, so the CI smoke
   flow and the gallery states behave the same under every skin. */
export interface ExecutionService {
  activeProfileName: string;
  executionView: ExecutionViewModel;
  manageService: (action: SystemServiceAction) => Promise<void>;
  profileIndicator: ProfileIndicator;
  refresh: () => Promise<void>;
  revealServiceLocation: () => Promise<void>;
  runSummaryAction: (command: SystemServiceAction) => void;
  service: SystemServiceStatus | undefined;
  serviceBusy: string | null;
  servicePackageNotice: ServicePackageNotice | null;
  serviceStateText: string;
  serviceStatusLine: ServiceStatusLine | null;
  serviceSummary: ServiceSummary;
  status: ExecutionStatus | null;
  systemInjectionAction: SystemInjectionPrimaryAction;
  toggleAutostart: (enabled: boolean) => Promise<void>;
}

export interface ExecutionLegacy {
  disableLegacyTrayStartup: () => Promise<void>;
  exitLegacyTray: () => Promise<void>;
  legacyService: LegacyMacTrayStatus | null | undefined;
  legacyTrayBusy: "exit" | "disable-autostart" | null;
  legacyTrayResolution: LegacyTrayResolution | null;
}

export interface ExecutionTargets {
  argumentsText: string;
  candidateFilter: string;
  candidates: readonly ManualLaunchCandidate[] | null;
  chooseTarget: () => Promise<void>;
  launch: () => Promise<void>;
  launchAll: () => Promise<void>;
  loadCandidates: () => Promise<void>;
  register: () => Promise<void>;
  remove: (registeredTarget: string) => Promise<void>;
  setArgumentsText: Dispatch<SetStateAction<string>>;
  setCandidateFilter: Dispatch<SetStateAction<string>>;
  setTarget: Dispatch<SetStateAction<string>>;
  target: string;
  targetName: string;
  visibleCandidates: ManualLaunchCandidate[];
}

export interface ExecutionMigration {
  closeMigrationConfirmation: () => void;
  confirmMigration: () => Promise<void>;
  handleMigrationDialogKeyDown: (event: ReactKeyboardEvent<HTMLElement>) => void;
  migrationCancelRef: RefObject<HTMLButtonElement | null>;
  migrationConfirmationOpen: boolean;
  migrationTriggerRef: RefObject<HTMLButtonElement | null>;
  openMigrationConfirmation: () => void;
}

export interface ExecutionMessages {
  error: string | null;
  message: string | null;
}

export interface ExecutionModel {
  service: ExecutionService;
  legacy: ExecutionLegacy;
  targets: ExecutionTargets;
  migration: ExecutionMigration;
  messages: ExecutionMessages;
}

export function useExecutionModel({ ciSmoke = false, onReady }: ExecutionModelOptions = {}): ExecutionModel {
  const { t } = useI18n();
  const [status, setStatus] = useState<ExecutionStatus | null>(null);
  const [target, setTarget] = useState("");
  const [argumentsText, setArgumentsText] = useState("");
  const [candidates, setCandidates] = useState<ReadonlyArray<ManualLaunchCandidate> | null>(null);
  const [candidateFilter, setCandidateFilter] = useState("");
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [serviceBusy, setServiceBusy] = useState<string | null>(null);
  const [legacyTrayBusy, setLegacyTrayBusy] = useState<"exit" | "disable-autostart" | null>(null);
  const [migrationConfirmationOpen, setMigrationConfirmationOpen] = useState(false);
  const migrationTriggerRef = useRef<HTMLButtonElement>(null);
  const migrationCancelRef = useRef<HTMLButtonElement>(null);
  /* Callers pass onReady as an inline closure, so it changes on every render.
     Reading it through a ref keeps refresh stable; otherwise each status
     answer re-rendered the caller, replaced refresh, and re-ran the status
     command without end, which kept the window thread busy (the Console
     skin, whose shell owns this model, moved with a 47 ms stall per step). */
  const onReadyRef = useRef(onReady);
  onReadyRef.current = onReady;

  useEffect(() => {
    if (migrationConfirmationOpen) migrationCancelRef.current?.focus();
  }, [migrationConfirmationOpen]);

  const refresh = useCallback(async () => {
    try {
      const nextStatus = await runtime().loadExecutionStatus();
      setStatus(nextStatus);
      setError(null);
      if (ciSmoke) {
        if (!nextStatus.injectionReady || !nextStatus.activeProfile) {
          throw new Error("CI profile application did not produce an active injection runtime");
        }
        await runtime().verifyInjectionWorkflowForCi();
        onReadyRef.current?.();
      }
    } catch (caught: unknown) {
      const message = caught instanceof Error ? caught.message : String(caught);
      setError(message);
      if (ciSmoke) void runtime().reportFrontendFailure("execution", message);
    }
  }, [ciSmoke]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const toggleAutostart = async (enabled: boolean) => {
    try {
      const actual = await runtime().setSessionAutostart(enabled);
      setStatus((current) => current ? { ...current, autoStart: actual } : current);
      setMessage(actual ? t("execution.autostartOn") : t("execution.autostartOff"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const argumentsFromEditor = () => argumentsText.split(/\r?\n/).map((argument) => argument.trim()).filter(Boolean);

  const launch = async () => {
    try {
      const pid = await runtime().launchTargetWithMactype(target, argumentsFromEditor());
      setMessage(t("execution.launched", { pid }));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const loadCandidates = useCallback(async () => {
    try {
      setCandidates(await runtime().listManualLaunchCandidates());
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  }, []);

  const chooseTarget = async () => {
    try {
      const selected = await runtime().pickExecutable(t("execution.executableFilter"));
      if (selected) setTarget(selected);
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const register = async () => {
    try {
      const sessionTargets = await runtime().registerSessionTarget(target, argumentsFromEditor());
      setStatus((current) => current ? { ...current, sessionTargets } : current);
      setMessage(t("execution.registered"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const remove = async (registeredTarget: string) => {
    try {
      const sessionTargets = await runtime().removeSessionTarget(registeredTarget);
      setStatus((current) => current ? { ...current, sessionTargets } : current);
      setMessage(t("execution.removed"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const launchAll = async () => {
    try {
      const processes = await runtime().launchRegisteredTargets();
      setMessage(t("execution.launchedRegistered", { count: processes.length }));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const manageService = async (action: SystemServiceAction) => {
    setServiceBusy(action);
    const hadProfile = Boolean(status?.activeProfile);
    try {
      const nextStatus = await runtime().manageSystemService(action);
      setStatus(nextStatus);
      const defaultApplied = !hadProfile && Boolean(nextStatus.activeProfile);
      const appliedName = nextStatus.activeProfile?.split(/[\\/]/).pop() ?? "";
      setMessage(
        action === "stop"
          ? t("execution.systemPaused")
          : action === "publish-profile"
            ? (defaultApplied
              ? t("execution.systemActivatedWithDefaultProfile", { name: appliedName })
              : t("execution.systemActivated"))
            : action === "migrate-from-legacy"
              ? t("execution.migrationComplete")
              : action === "remove-legacy"
                ? t("execution.legacyRemoved")
                : action === "start" && defaultApplied
                  ? t("execution.serviceStartedWithDefaultProfile", { name: appliedName })
                  : t("execution.serviceActionDone"),
      );
      setError(null);
    } catch (caught: unknown) {
      setError(operationErrorMessage(
        caught,
        t,
        action === "migrate-from-legacy" ? "execution.migrationFailed" : "execution.operationFailed",
      ));
      setMessage(null);
    } finally {
      setServiceBusy(null);
    }
  };

  const revealServiceLocation = async () => {
    try {
      await runtime().revealSystemService();
      setMessage(t("execution.serviceLocationOpened"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
      setMessage(null);
    }
  };

  const exitLegacyTray = async () => {
    const process = status?.legacyTray.process;
    if (!process || process.state !== "trusted-current-session") return;
    setLegacyTrayBusy("exit");
    try {
      const nextStatus = await runtime().requestLegacyTrayExit({
        pid: process.pid,
        creationTime: process.creationTime,
        path: process.path,
      });
      setStatus(nextStatus);
      setMessage(t("execution.legacyTrayExited"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
      setMessage(null);
    } finally {
      setLegacyTrayBusy(null);
    }
  };

  const disableLegacyTrayStartup = async () => {
    setLegacyTrayBusy("disable-autostart");
    try {
      const nextStatus = await runtime().disableLegacyTrayAutostart();
      setStatus(nextStatus);
      setMessage(t("execution.legacyTrayAutostartDisabled"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
      setMessage(null);
    } finally {
      setLegacyTrayBusy(null);
    }
  };

  const restoreMigrationTriggerFocus = () => {
    window.requestAnimationFrame(() => migrationTriggerRef.current?.focus());
  };

  const openMigrationConfirmation = () => setMigrationConfirmationOpen(true);

  const closeMigrationConfirmation = () => {
    setMigrationConfirmationOpen(false);
    restoreMigrationTriggerFocus();
  };

  const confirmMigration = async () => {
    setMigrationConfirmationOpen(false);
    await manageService("migrate-from-legacy");
    restoreMigrationTriggerFocus();
  };

  const handleMigrationDialogKeyDown = (event: ReactKeyboardEvent<HTMLElement>) => {
    if (event.key === "Escape") {
      event.preventDefault();
      closeMigrationConfirmation();
      return;
    }
    if (event.key !== "Tab") return;
    const focusable = [...event.currentTarget.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")];
    const first = focusable[0];
    const last = focusable.at(-1);
    if (!first || !last) return;
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  };

  const candidateFilterText = candidateFilter.trim().toLowerCase();
  const visibleCandidates = (candidates ?? []).filter((candidate) => !candidateFilterText
    || candidate.name.toLowerCase().includes(candidateFilterText)
    || candidate.path.toLowerCase().includes(candidateFilterText)
    || (candidate.windowTitle?.toLowerCase().includes(candidateFilterText) ?? false));

  const executionView = useMemo(() => projectExecutionView(status, serviceBusy), [serviceBusy, status]);
  const systemInjectionAction = executionView.systemInjectionAction;
  const service = executionView.status?.systemService;
  const legacyService = executionView.status?.legacyMacTray;
  const legacyTrayResolution = executionView.legacyTrayResolution;
  const serviceSummary = executionView.serviceSummary;
  const serviceStatusLine = executionView.serviceStatusLine;
  const profileIndicator = executionView.profileIndicator;
  const serviceStateText = serviceStatusLine
    ? [serviceStatusLine.installationKey, serviceStatusLine.runtimeKey, ...(serviceStatusLine.healthKey ? [serviceStatusLine.healthKey] : [])].map((key) => t(key)).join(" · ")
    : t("execution.checking");
  const servicePackageNotice = executionView.servicePackageNotice;
  const activeProfileName = executionView.activeProfileDisplay.name ?? t(executionView.activeProfileDisplay.fallbackKey);
  const targetName = target ? target.split(/[\\/]/).pop() ?? target : "";

  const runSummaryAction = (command: SystemServiceAction) => {
    if (command === "migrate-from-legacy") {
      setMigrationConfirmationOpen(true);
      return;
    }
    void manageService(command);
  };

  return {
    service: {
      activeProfileName,
      executionView,
      manageService,
      profileIndicator,
      refresh,
      revealServiceLocation,
      runSummaryAction,
      service,
      serviceBusy,
      servicePackageNotice,
      serviceStateText,
      serviceStatusLine,
      serviceSummary,
      status,
      systemInjectionAction,
      toggleAutostart,
    },
    legacy: {
      disableLegacyTrayStartup,
      exitLegacyTray,
      legacyService,
      legacyTrayBusy,
      legacyTrayResolution,
    },
    targets: {
      argumentsText,
      candidateFilter,
      candidates,
      chooseTarget,
      launch,
      launchAll,
      loadCandidates,
      register,
      remove,
      setArgumentsText,
      setCandidateFilter,
      setTarget,
      target,
      targetName,
      visibleCandidates,
    },
    migration: {
      closeMigrationConfirmation,
      confirmMigration,
      handleMigrationDialogKeyDown,
      migrationCancelRef,
      migrationConfirmationOpen,
      migrationTriggerRef,
      openMigrationConfirmation,
    },
    messages: {
      error,
      message,
    },
  };
}
