import { AlertTriangle, Check, FileCode2, FolderOpen, Play, RefreshCw, ShieldAlert, Trash2, UserPlus } from "lucide-react";
import { useCallback, useEffect, useState } from "react";
import type { ExecutionStatus } from "../app/model";
import { launchRegisteredTargets, launchTargetWithMactype, loadExecutionStatus, manageLegacyService, pickExecutable, registerSessionTarget, removeSessionTarget, reportFrontendFailure, setSessionAutostart, verifyInjectionWorkflowForCi } from "../app/tauri";
import { useI18n } from "../i18n/i18n";

export function ExecutionPage({ ciSmoke = false, onReady }: { ciSmoke?: boolean; onReady?: () => void }) {
  const { t } = useI18n();
  const [status, setStatus] = useState<ExecutionStatus | null>(null);
  const [target, setTarget] = useState("");
  const [argumentsText, setArgumentsText] = useState("");
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [serviceBusy, setServiceBusy] = useState<string | null>(null);

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

  const manageService = async (action: "install" | "remove" | "start" | "stop") => {
    setServiceBusy(action);
    try {
      const legacyService = await manageLegacyService(action);
      setStatus((current) => current ? { ...current, legacyService } : current);
      setMessage(t("execution.serviceActionDone"));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
      setMessage(null);
    } finally {
      setServiceBusy(null);
    }
  };

  const service = status?.legacyService;

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
        <dl className="detail-list">
          <div><dt>{t("execution.legacyService")}</dt><dd>{service?.presence === "owned" ? <Check className="success" size={17} /> : <AlertTriangle className="warning" size={17} />}<span>{service ? `${t(`execution.servicePresence.${service.presence}`)} · ${t(`execution.serviceState.${service.state}`)}` : t("execution.checking")}</span></dd></div>
          <div><dt>{t("execution.appInit")}</dt><dd>{status?.registryModeDetected ? <ShieldAlert className="warning" size={17} /> : <Check className="success" size={17} />}<span>{status?.registryModeDetected ? t("execution.entryDetected") : t("profiles.disabled")}</span></dd></div>
        </dl>
        <div className="service-controls">
          <div>
            <strong>{t("execution.serviceControlTitle")}</strong>
            <p>{t("execution.serviceControlDescription")}</p>
            {service?.binaryPath && <code title={service.binaryPath}>{service.binaryPath}</code>}
            {service?.registryConflict && <p className="warning-text">{t("execution.serviceRegistryConflict")}</p>}
            {service?.presence === "foreign" && <p className="warning-text">{t("execution.serviceForeign")}</p>}
          </div>
          <div className="service-actions">
            <button className="button secondary" disabled={!service?.canInstall || serviceBusy !== null} onClick={() => void manageService("install")} type="button">{serviceBusy === "install" ? t("execution.serviceWorking") : t("execution.serviceInstall")}</button>
            <button className="button secondary" disabled={!service?.canStart || serviceBusy !== null} onClick={() => void manageService("start")} type="button">{serviceBusy === "start" ? t("execution.serviceWorking") : t("execution.serviceStart")}</button>
            <button className="button secondary" disabled={!service?.canStop || serviceBusy !== null} onClick={() => void manageService("stop")} type="button">{serviceBusy === "stop" ? t("execution.serviceWorking") : t("execution.serviceStop")}</button>
            <button className="button secondary danger" disabled={!service?.canRemove || serviceBusy !== null} onClick={() => void manageService("remove")} type="button">{serviceBusy === "remove" ? t("execution.serviceWorking") : t("execution.serviceRemove")}</button>
          </div>
        </div>
        <div className="system-mode-note"><ShieldAlert aria-hidden="true" size={19} /><p>{status ? t("execution.systemNote") : t("execution.checking")}</p></div>
      </section>

      {message && <p className="success-message">{message}</p>}
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
    </section>
  );
}
