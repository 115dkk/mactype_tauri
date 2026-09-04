import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent as ReactKeyboardEvent } from "react";
import type { ExecutionStatus, ManualLaunchCandidate, SystemServiceAction } from "../../app/model";
import { projectExecutionView } from "../../app/executionViewModel";
import { operationErrorMessage } from "../../app/operationError";
import { disableLegacyTrayAutostart, launchRegisteredTargets, launchTargetWithMactype, listManualLaunchCandidates, loadExecutionStatus, manageSystemService, pickExecutable, registerSessionTarget, removeSessionTarget, reportFrontendFailure, requestLegacyTrayExit, revealSystemService, setSessionAutostart, verifyInjectionWorkflowForCi } from "../../app/tauri";
import { useI18n, type MessageKey } from "../../i18n/i18n";

export interface ExecutionModelOptions {
  ciSmoke?: boolean;
  onReady?: () => void;
}

export interface ServicePackageNoticeKeys {
  titleKey: MessageKey;
  descriptionKey: MessageKey;
}

/* The service page state machine, shared by every skin. A skin composes the
   rows, toolbars and dialogs differently, but the actions, busy flags,
   messages and the projected view come from this one hook, so the CI smoke
   flow and the gallery states behave the same under every skin. */
export function useExecutionModel({ ciSmoke = false, onReady }: ExecutionModelOptions = {}) {
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

  useEffect(() => {
    if (migrationConfirmationOpen) migrationCancelRef.current?.focus();
  }, [migrationConfirmationOpen]);

  const refresh = useCallback(async () => {
    try {
      const nextStatus = await loadExecutionStatus();
      setStatus(nextStatus);
      setError(null);
      if (ciSmoke) {
        if (!nextStatus.injectionReady || !nextStatus.activeProfile) {
          throw new Error("CI profile application did not produce an active injection runtime");
        }
        await verifyInjectionWorkflowForCi();
        onReady?.();
      }
    } catch (caught: unknown) {
      const message = caught instanceof Error ? caught.message : String(caught);
      setError(message);
      if (ciSmoke) void reportFrontendFailure("execution", message);
    }
  }, [ciSmoke, onReady]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const toggleAutostart = async (enabled: boolean) => {
    try {
      const actual = await setSessionAutostart(enabled);
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
      const pid = await launchTargetWithMactype(target, argumentsFromEditor());
      setMessage(t("execution.launched", { pid }));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const loadCandidates = useCallback(async () => {
    try {
      setCandidates(await listManualLaunchCandidates());
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  }, []);

  const chooseTarget = async () => {
    try {
      const selected = await pickExecutable(t("execution.executableFilter"));
      if (selected) setTarget(selected);
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const register = async () => {
    try {
      const sessionTargets = await registerSessionTarget(target, argumentsFromEditor());
      setStatus((current) => current ? { ...current, sessionTargets } : current);
      setMessage(t("execution.registered"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const remove = async (registeredTarget: string) => {
    try {
      const sessionTargets = await removeSessionTarget(registeredTarget);
      setStatus((current) => current ? { ...current, sessionTargets } : current);
      setMessage(t("execution.removed"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const launchAll = async () => {
    try {
      const processes = await launchRegisteredTargets();
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
      const nextStatus = await manageSystemService(action);
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
      await revealSystemService();
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
      const nextStatus = await requestLegacyTrayExit({
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
      const nextStatus = await disableLegacyTrayAutostart();
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
  const servicePackageNotice: ServicePackageNoticeKeys | null = status?.serviceManagementPackage === "not-installed"
    ? { titleKey: "execution.servicePackageNotInstalledTitle", descriptionKey: "execution.servicePackageNotInstalledDescription" }
    : status?.serviceManagementPackage === "incomplete"
      ? { titleKey: "execution.servicePackageIncompleteTitle", descriptionKey: "execution.servicePackageIncompleteDescription" }
      : status?.serviceManagementPackage === "untrusted"
        ? { titleKey: "execution.servicePackageUntrustedTitle", descriptionKey: "execution.servicePackageUntrustedDescription" }
        : null;
  const activeProfileName = status?.activeProfile?.split(/[\\/]/).pop() ?? t("execution.profileNotApplied");
  const targetName = target ? target.split(/[\\/]/).pop() ?? target : "";

  const runSummaryAction = (command: SystemServiceAction) => {
    if (command === "migrate-from-legacy") {
      setMigrationConfirmationOpen(true);
      return;
    }
    void manageService(command);
  };

  return {
    activeProfileName,
    argumentsText,
    candidateFilter,
    candidates,
    chooseTarget,
    closeMigrationConfirmation,
    confirmMigration,
    disableLegacyTrayStartup,
    error,
    executionView,
    exitLegacyTray,
    handleMigrationDialogKeyDown,
    launch,
    launchAll,
    legacyService,
    legacyTrayBusy,
    legacyTrayResolution,
    loadCandidates,
    manageService,
    message,
    migrationCancelRef,
    migrationConfirmationOpen,
    migrationTriggerRef,
    openMigrationConfirmation,
    profileIndicator,
    refresh,
    register,
    remove,
    revealServiceLocation,
    runSummaryAction,
    service,
    serviceBusy,
    servicePackageNotice,
    serviceStateText,
    serviceStatusLine,
    serviceSummary,
    setArgumentsText,
    setCandidateFilter,
    setTarget,
    status,
    systemInjectionAction,
    t,
    target,
    targetName,
    toggleAutostart,
    visibleCandidates,
  };
}

export type ExecutionModel = ReturnType<typeof useExecutionModel>;
