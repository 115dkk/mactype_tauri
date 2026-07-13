import { AlertTriangle, Check, Play, RefreshCw, ShieldAlert, Trash2, UserPlus } from "lucide-react";
import { useCallback, useEffect, useState } from "react";
import type { ExecutionStatus } from "../app/model";
import { launchRegisteredTargets, launchTargetWithMactype, loadExecutionStatus, registerSessionTarget, removeSessionTarget, reportFrontendFailure, setSessionAutostart, verifyInjectionWorkflowForCi } from "../app/tauri";
import { useI18n } from "../i18n/i18n";

export function ExecutionPage({ ciSmoke = false, onReady }: { ciSmoke?: boolean; onReady?: () => void }) {
  const { t } = useI18n();
  const [status, setStatus] = useState<ExecutionStatus | null>(null);
  const [target, setTarget] = useState("");
  const [argumentsText, setArgumentsText] = useState("");
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

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
          <label><span>{t("execution.path")}</span><input onChange={(event) => setTarget(event.target.value)} placeholder="C:\\Windows\\System32\\notepad.exe" type="text" value={target} /></label>
          <label><span>{t("execution.arguments")}</span><textarea onChange={(event) => setArgumentsText(event.target.value)} placeholder={t("execution.argumentsPlaceholder")} rows={3} value={argumentsText} /></label>
          <div className="manual-actions"><button className="button secondary" disabled={!status?.injectionReady || !target.trim()} onClick={() => void register()} type="button"><UserPlus aria-hidden="true" size={17} /> {t("execution.register")}</button><button className="button primary" disabled={!status?.manualLauncherAvailable || !target.trim()} onClick={() => void launch()} type="button"><Play aria-hidden="true" size={17} /> {t("execution.launch")}</button></div>
        </div>
      </section>

      <section className="section-block" aria-labelledby="system-title">
        <div className="section-heading"><div><h2 id="system-title">{t("execution.systemTitle")}</h2><p>{t("execution.systemDescription")}</p></div></div>
        <dl className="detail-list">
          <div><dt>{t("execution.legacyService")}</dt><dd>{status?.legacyServiceDetected ? <AlertTriangle className="warning" size={17} /> : <Check className="success" size={17} />}<span>{status?.legacyServiceDetected ? t("execution.detected", { state: status.legacyServiceRunning ? t("execution.running") : t("execution.stopped") }) : t("execution.notDetected")}</span></dd></div>
          <div><dt>{t("execution.appInit")}</dt><dd>{status?.registryModeDetected ? <ShieldAlert className="warning" size={17} /> : <Check className="success" size={17} />}<span>{status?.registryModeDetected ? t("execution.entryDetected") : t("profiles.disabled")}</span></dd></div>
        </dl>
        <div className="system-mode-note"><ShieldAlert aria-hidden="true" size={19} /><p>{status ? t("execution.systemNote") : t("execution.checking")}</p></div>
      </section>

      {message && <p className="success-message">{message}</p>}
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
    </section>
  );
}
