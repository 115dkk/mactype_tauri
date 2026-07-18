import { AlertTriangle, Check, FileCode2, FolderOpen, Play, Power, PowerOff, RefreshCw, ShieldAlert, Trash2, UserPlus } from "lucide-react";
import { useCallback, useEffect, useRef, useState, type KeyboardEvent as ReactKeyboardEvent } from "react";
import type { ExecutionStatus, SystemServiceAction } from "../app/model";
import { projectExecutionView } from "../app/executionViewModel";
import { launchRegisteredTargets, launchTargetWithMactype, loadExecutionStatus, manageSystemService, pickExecutable, registerSessionTarget, removeSessionTarget, reportFrontendFailure, revealSystemService, setSessionAutostart, verifyInjectionWorkflowForCi } from "../app/tauri";
import { useI18n } from "../i18n/i18n";

export function ExecutionPage({ ciSmoke = false, onReady }: { ciSmoke?: boolean; onReady?: () => void }) {
  const { t } = useI18n();
  const [status, setStatus] = useState<ExecutionStatus | null>(null);
  const [target, setTarget] = useState("");
  const [argumentsText, setArgumentsText] = useState("");
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [serviceBusy, setServiceBusy] = useState<string | null>(null);
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

  const launch = async () => {
    try {
      const arguments_ = argumentsText.split(/\r?\n/).map((argument) => argument.trim()).filter(Boolean);
      const pid = await launchTargetWithMactype(target, arguments_);
      setMessage(t("execution.launched", { pid }));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const argumentsFromEditor = () => argumentsText.split(/\r?\n/).map((argument) => argument.trim()).filter(Boolean);

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
    try {
      const nextStatus = await manageSystemService(action);
      setStatus(nextStatus);
      setMessage(
        action === "stop"
          ? t("execution.systemPaused")
          : action === "publish-profile"
            ? t("execution.systemActivated")
            : action === "migrate-from-legacy"
              ? t("execution.migrationComplete")
              : action === "remove-legacy"
                ? t("execution.legacyRemoved")
                : t("execution.serviceActionDone"),
      );
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
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

  const restoreMigrationTriggerFocus = () => {
    window.requestAnimationFrame(() => migrationTriggerRef.current?.focus());
  };

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

  const executionView = projectExecutionView(status, serviceBusy);
  const systemInjectionAction = executionView.systemInjectionAction;
  const service = executionView.status?.systemService;
  const legacyService = executionView.status?.legacyMacTray;

  return (
    <section className="page view-enter" aria-labelledby="execution-title">
      <header className="page-header">
        <div><h1 id="execution-title">{t("nav.execution")}</h1><p>{t("execution.subtitle")}</p></div>
        <button className="button secondary" onClick={() => void refresh()} type="button"><RefreshCw aria-hidden="true" size={16} /> {t("execution.refresh")}</button>
      </header>

      <section className="section-block" aria-labelledby="tray-title">
        <div className="section-heading"><div><h2 id="tray-title">{t("execution.trayTitle")}</h2><p>{t("execution.trayDescription")}</p></div></div>
        <div className="execution-option">
          <div>{status?.trayAvailable ? <Check className="success" aria-hidden="true" size={18} /> : <AlertTriangle className="warning" aria-hidden="true" size={18} />}<div><strong>{t("execution.autostartTitle")}</strong><p>{t("execution.autostartDescription")}</p></div></div>
          <label className="switch-control"><input checked={status?.autoStart ?? false} disabled={!status} onChange={(event) => void toggleAutostart(event.target.checked)} type="checkbox" /><span>{status?.autoStart ? t("profiles.enabled") : t("profiles.disabled")}</span></label>
        </div>
        <dl className="detail-list">
          <div><dt>{t("execution.activeProfile")}</dt><dd>{status?.injectionReady ? <Check className="success" size={17} /> : <AlertTriangle className="warning" size={17} />}<code>{status?.activeProfile ?? t("execution.profileNotApplied")}</code></dd></div>
        </dl>
        <div className="registered-launchers">
          <div className="registered-heading"><div><strong>{t("execution.registeredTitle")}</strong><p>{t("execution.registeredDescription")}</p></div><button className="button secondary" disabled={!status?.injectionReady || !status.sessionTargets.length} onClick={() => void launchAll()} type="button"><Play aria-hidden="true" size={16} /> {t("execution.launchRegistered")}</button></div>
          {status?.sessionTargets.length ? <ul>{status.sessionTargets.map((entry) => <li key={entry.target}><code>{entry.target}</code><button aria-label={t("execution.removeTarget", { name: entry.target })} className="icon-button" onClick={() => void remove(entry.target)} type="button"><Trash2 aria-hidden="true" size={16} /></button></li>)}</ul> : <p className="empty-state">{t("execution.noRegistered")}</p>}
        </div>
      </section>

      <section className="section-block" aria-labelledby="manual-title">
        <div className="section-heading"><div><h2 id="manual-title">{t("execution.manualTitle")}</h2><p>{t("execution.manualDescription")}</p></div></div>
        <div className="manual-launcher">
          <div className="target-picker">
            <span>{t("execution.path")}</span>
            <div className="target-selection" data-empty={!target}>
              <FileCode2 aria-hidden="true" size={22} />
              <div>
                <strong>{target.split(/[\\/]/).pop() || t("execution.noExecutableSelected")}</strong>
                {target && <code title={target}>{target}</code>}
              </div>
              <button className="button secondary" onClick={() => void chooseTarget()} type="button"><FolderOpen aria-hidden="true" size={17} /> {target ? t("execution.changeExecutable") : t("execution.chooseExecutable")}</button>
            </div>
          </div>
          <label><span>{t("execution.arguments")}</span><textarea onChange={(event) => setArgumentsText(event.target.value)} placeholder={t("execution.argumentsPlaceholder")} rows={3} value={argumentsText} /></label>
          <div className="manual-actions"><button className="button secondary" disabled={!status?.injectionReady || !target.trim()} onClick={() => void register()} type="button"><UserPlus aria-hidden="true" size={17} /> {t("execution.register")}</button><button className="button primary" disabled={!status?.manualLauncherAvailable || !target.trim()} onClick={() => void launch()} type="button"><Play aria-hidden="true" size={17} /> {t("execution.launch")}</button></div>
        </div>
      </section>

      <section className="section-block" aria-labelledby="system-title">
        <div className="section-heading"><div><h2 id="system-title">{t("execution.systemTitle")}</h2><p>{t("execution.systemDescription")}</p></div></div>
        <div className="open-service-card" data-service-backend="open-source">
        <div className="system-injection-control" data-active={systemInjectionAction.state === "active"} data-state={systemInjectionAction.state}>
          <div className="system-injection-state">
            <span className="system-injection-icon">{systemInjectionAction.intent === "stop" ? <Power aria-hidden="true" size={20} /> : <PowerOff aria-hidden="true" size={20} />}</span>
            <div>
              <span className="eyebrow">{t("execution.openServiceTitle")}</span>
              <strong>{t(systemInjectionAction.titleKey)}</strong>
              <p>{t(systemInjectionAction.descriptionKey)}</p>
            </div>
          </div>
          <button
            className={systemInjectionAction.intent === "stop" ? "button secondary system-injection-action" : "button primary system-injection-action"}
            disabled={!systemInjectionAction.enabled}
            onClick={() => void manageService(systemInjectionAction.command)}
            type="button"
          >
            {t(systemInjectionAction.labelKey)}
          </button>
        </div>
        <dl className="detail-list">
          <div><dt>{t("execution.openServiceStatus")}</dt><dd>{systemInjectionAction.state === "active" ? <Check className="success" size={17} /> : <AlertTriangle className="warning" size={17} />}<span>{service ? `${t(`execution.installation.${service.installation}`)} · ${t(`execution.serviceState.${service.runtime}`)} · ${t(`execution.health.${service.health}`)}` : t("execution.checking")}</span></dd></div>
          <div><dt>{t("execution.profileGeneration")}</dt><dd>{executionView.profileMatches ? <Check className="success" size={17} /> : <AlertTriangle className="warning" size={17} />}<span>{executionView.profileMatches ? t("execution.profileMatched") : t("execution.profileNotMatched")}</span></dd></div>
          <div><dt>{t("execution.appInit")}</dt><dd>{status?.registryModeDetected ? <ShieldAlert className="warning" size={17} /> : <Check className="success" size={17} />}<span>{status?.registryModeDetected ? t("execution.entryDetected") : t("profiles.disabled")}</span></dd></div>
        </dl>
        <div className="service-controls">
          <div>
            <strong>{t("execution.openServiceControlTitle")}</strong>
            <p>{t("execution.openServiceControlDescription")}</p>
            {executionView.serviceBinaryPath && (
              <div className="service-path">
                <code title={executionView.serviceBinaryPath}>{executionView.serviceBinaryPath}</code>
                <button className="button secondary" onClick={() => void revealServiceLocation()} type="button">
                  <FolderOpen aria-hidden="true" size={16} /> {t("execution.revealSystemService")}
                </button>
              </div>
            )}
            {service?.backend === "foreign" && <p className="warning-text">{t("execution.serviceForeign")}</p>}
          </div>
          <div className="service-actions">
            <button className="button secondary" disabled={!executionView.canInstall} onClick={() => void manageService("install")} type="button">{serviceBusy === "install" ? t("execution.serviceWorking") : t("execution.serviceInstall")}</button>
            <button className="button secondary" disabled={!executionView.canStart} onClick={() => void manageService("start")} type="button">{serviceBusy === "start" ? t("execution.serviceWorking") : t("execution.serviceStart")}</button>
            {executionView.serviceNeedsUpgrade && <button className="button secondary" disabled={!executionView.canUpgrade} onClick={() => void manageService("upgrade")} type="button">{serviceBusy === "upgrade" ? t("execution.serviceWorking") : t("execution.serviceUpgrade")}</button>}
            {executionView.serviceNeedsRepair && <button className="button secondary" disabled={!executionView.canRepair} onClick={() => void manageService("repair")} type="button">{serviceBusy === "repair" ? t("execution.serviceWorking") : t("execution.serviceRepair")}</button>}
            <button className="button secondary danger" disabled={!executionView.canRemove} onClick={() => void manageService("remove")} type="button">{serviceBusy === "remove" ? t("execution.serviceWorking") : t("execution.serviceRemove")}</button>
          </div>
        </div>
        </div>
        {legacyService && (
          <div className="service-controls legacy-service-controls" data-service-backend="legacy-mactray">
            <div>
              <strong>{t("execution.legacyServiceTitle")}</strong>
              <p>{t("execution.legacyServiceDescription")}</p>
              <span>{`${t(`execution.servicePresence.${legacyService.presence}`)} · ${t(`execution.serviceState.${legacyService.state}`)}`}</span>
              {legacyService.registryConflict && <p className="warning-text">{t("execution.serviceRegistryConflict")}</p>}
              {legacyService.presence === "foreign" && <p className="warning-text">{t("execution.serviceForeign")}</p>}
            </div>
            <div className="service-actions">
              <button className="button secondary" disabled={!executionView.canMigrateLegacy} onClick={() => setMigrationConfirmationOpen(true)} ref={migrationTriggerRef} type="button">{serviceBusy === "migrate-from-legacy" ? t("execution.serviceWorking") : t("execution.migrateLegacy")}</button>
              <button className="button secondary danger" disabled={!executionView.canRemoveLegacy} onClick={() => void manageService("remove-legacy")} type="button">{serviceBusy === "remove-legacy" ? t("execution.serviceWorking") : t("execution.removeLegacy")}</button>
            </div>
          </div>
        )}
        <div className="system-mode-note"><ShieldAlert aria-hidden="true" size={19} /><p>{status ? t("execution.systemNote") : t("execution.checking")}</p></div>
      </section>

      {message && <p className="success-message">{message}</p>}
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
      {migrationConfirmationOpen && (
        <div className="confirmation-backdrop">
          <section
            aria-labelledby="migration-confirmation-title"
            aria-modal="true"
            className="migration-confirmation"
            onKeyDown={handleMigrationDialogKeyDown}
            role="dialog"
          >
            <div className="migration-confirmation-heading">
              <ShieldAlert aria-hidden="true" size={22} />
              <div>
                <h2 id="migration-confirmation-title">{t("execution.migrationConfirmTitle")}</h2>
                <p>{t("execution.migrationConfirmDescription")}</p>
              </div>
            </div>
            <ol>
              <li>{t("execution.migrationConfirmStrictCheck")}</li>
              <li>{t("execution.migrationConfirmBackup")}</li>
              <li>{t("execution.migrationConfirmSwitch")}</li>
              <li>{t("execution.migrationConfirmVerify")}</li>
              <li>{t("execution.migrationConfirmRollback")}</li>
            </ol>
            <p className="migration-confirmation-note">{t("execution.migrationConfirmRemoval")}</p>
            <div className="migration-confirmation-actions">
              <button className="button secondary" onClick={closeMigrationConfirmation} ref={migrationCancelRef} type="button">{t("execution.migrationCancel")}</button>
              <button className="button primary" disabled={!executionView.canMigrateLegacy} onClick={() => void confirmMigration()} type="button">{t("execution.migrationContinue")}</button>
            </div>
          </section>
        </div>
      )}
    </section>
  );
}
