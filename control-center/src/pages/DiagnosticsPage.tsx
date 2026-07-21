import { AlertTriangle, Check, Copy, Download, ExternalLink, LoaderCircle } from "lucide-react";
import { useEffect, useState } from "react";
import type { InstallationStatus } from "../app/model";
import { copyDiagnostics, exportDiagnostics, loadDiagnosticLogs, openLogFolder } from "../app/tauri";
import { useI18n } from "../i18n/i18n";

export function DiagnosticsPage({ status }: { status: InstallationStatus }) {
  const { t } = useI18n();
  const [operationLogs, setOperationLogs] = useState<ReadonlyArray<string>>([]);
  const [operation, setOperation] = useState<"export" | "copy" | "folder" | null>(null);
  const [completed, setCompleted] = useState<{ kind: "export" | "copy" | "folder"; detail: string } | null>(null);
  const [error, setError] = useState<string | null>(null);

  const run = async (kind: "export" | "copy" | "folder") => {
    setOperation(kind);
    setCompleted(null);
    setError(null);
    try {
      if (kind === "export") setCompleted({ kind, detail: await exportDiagnostics() });
      if (kind === "copy") {
        await copyDiagnostics();
        setCompleted({ kind, detail: t("diagnostics.copy") });
      }
      if (kind === "folder") setCompleted({ kind, detail: await openLogFolder() });
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    } finally {
      setOperation(null);
    }
  };

  useEffect(() => {
    let active = true;
    void loadDiagnosticLogs()
      .then((entries) => {
        if (active) setOperationLogs(entries);
      })
      .catch((caught: unknown) => {
        if (active) setError(caught instanceof Error ? caught.message : String(caught));
      });
    return () => {
      active = false;
    };
  }, []);

  return (
    <section className="page view-enter" aria-labelledby="diagnostics-title">
      <header className="page-header">
        <div><h1 id="diagnostics-title">{t("nav.diagnostics")}</h1><p>{t("diagnostics.subtitle")}</p></div>
        <button aria-busy={operation === "export"} className="button primary" disabled={operation !== null} onClick={() => void run("export")} type="button">{operation === "export" ? <LoaderCircle aria-hidden="true" className="spin" size={17} /> : <Download aria-hidden="true" size={17} />} {t("diagnostics.export")}</button>
      </header>
      <section className="section-block" aria-labelledby="components-title">
        <div className="section-heading"><h2 id="components-title">{t("diagnostics.components")}</h2><button aria-busy={operation === "copy"} className="icon-button" disabled={operation !== null} aria-label={t("diagnostics.copy")} onClick={() => void run("copy")} type="button">{operation === "copy" ? <LoaderCircle aria-hidden="true" className="spin" size={17} /> : <Copy aria-hidden="true" size={17} />}</button></div>
        <dl className="detail-list diagnostic-list">
          <div><dt>Control Center</dt><dd><Check className="success" size={17} /><code>0.1.0</code></dd></div>
          <div><dt>{t("diagnostics.core")}</dt><dd><Check className="success" size={17} /><code>{status.coreVersion ?? t("diagnostics.unknown")}</code></dd></div>
        </dl>
      </section>
      <section className="section-block" aria-labelledby="log-title">
        <div className="section-heading"><div><h2 id="log-title">{t("diagnostics.logs")}</h2><p>{t("diagnostics.logsDescription")}</p></div><button aria-busy={operation === "folder"} className="text-action" disabled={operation !== null} onClick={() => void run("folder")} type="button">{t("diagnostics.openFolder")} {operation === "folder" ? <LoaderCircle aria-hidden="true" className="spin" size={15} /> : <ExternalLink aria-hidden="true" size={15} />}</button></div>
        <div className="log-view" role="log" aria-label={t("diagnostics.logAria")}>
          {operationLogs.length === 0 ? (
            <div><time>{t("diagnostics.now")}</time><span>{t("diagnostics.logs")}</span><p>{t("diagnostics.noOperationLogs")}</p></div>
          ) : operationLogs.slice(-20).map((entry, index) => (
            <div data-severity="warning" key={`${index}-${entry}`}><time>{t("diagnostics.recent")}</time><span>{t("diagnostics.logs")}</span><p>{entry}</p></div>
          ))}
        </div>
      </section>
      <div aria-live="polite">
        {completed && <p className="success-message" data-operation={completed.kind}><Check aria-hidden="true" size={16} /> <span>{completed.detail}</span></p>}
        {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
      </div>
    </section>
  );
}
