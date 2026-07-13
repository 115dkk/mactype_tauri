import { AlertTriangle, Check, Play, RefreshCw, ShieldAlert } from "lucide-react";
import { useCallback, useEffect, useState } from "react";
import type { ExecutionStatus } from "../app/model";
import { launchTargetWithMactype, loadExecutionStatus, setSessionAutostart } from "../app/tauri";
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
      setStatus(await loadExecutionStatus());
      setError(null);
      if (ciSmoke) onReady?.();
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
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
      </section>

      <section className="section-block" aria-labelledby="manual-title">
        <div className="section-heading"><div><h2 id="manual-title">{t("execution.manualTitle")}</h2><p>{t("execution.manualDescription")}</p></div></div>
        <div className="manual-launcher">
          <label><span>{t("execution.path")}</span><input onChange={(event) => setTarget(event.target.value)} placeholder="C:\\Windows\\System32\\notepad.exe" type="text" value={target} /></label>
          <label><span>{t("execution.arguments")}</span><textarea onChange={(event) => setArgumentsText(event.target.value)} placeholder={t("execution.argumentsPlaceholder")} rows={3} value={argumentsText} /></label>
          <button className="button primary" disabled={!status?.manualLauncherAvailable || !target.trim()} onClick={() => void launch()} type="button"><Play aria-hidden="true" size={17} /> {t("execution.launch")}</button>
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
