import { AlertTriangle, Check, ChevronDown, ChevronUp, Copy, Download, ExternalLink, FolderSearch, LoaderCircle } from "lucide-react";
import { useEffect, useState } from "react";
import type { InstallationStatus } from "../app/model";
import { copyDiagnostics, exportDiagnostics, loadDiagnosticLogs, openLogFolder } from "../app/tauri";
import { useI18n } from "../i18n/i18n";

interface DiagnosticsPageProps {
  status: InstallationStatus;
  onReconnect: () => Promise<InstallationStatus>;
  onRelocate: () => Promise<InstallationStatus>;
}

type Operation = "export" | "copy" | "folder" | "relocate" | "reconnect";

export function DiagnosticsPage({ status, onReconnect, onRelocate }: DiagnosticsPageProps) {
  const { t } = useI18n();
  const [operationLogs, setOperationLogs] = useState<ReadonlyArray<string>>([]);
  const [logsExpanded, setLogsExpanded] = useState(false);
  const [operation, setOperation] = useState<Operation | null>(null);
  const [completed, setCompleted] = useState<{ kind: Operation; detail: string } | null>(null);
  const [error, setError] = useState<string | null>(null);

  const run = async (kind: Operation) => {
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
      if (kind === "relocate") {
        await onRelocate();
        setCompleted({ kind, detail: t("overview.relocate") });
      }
      if (kind === "reconnect") {
        await onReconnect();
        setCompleted({ kind, detail: t("overview.reconnect") });
      }
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    } finally {
      setOperation(null);
    }
  };
  const findingLabel = (label: string, value: string) => {
    if (value === "MacType.dll") return t("finding.core32");
    if (value === "MacType64.dll") return t("finding.core64");
    if (value === "MacLoader.exe") return t("finding.loader");
    if (label === "preview") return t("finding.preview");
    return label;
  };

  useEffect(() => {
    let active = true;
    void loadDiagnosticLogs().then((entries) => {
      if (active) setOperationLogs(entries);
    }).catch((caught: unknown) => {
      if (active) setError(caught instanceof Error ? caught.message : String(caught));
    });
    return () => { active = false; };
  }, []);

  return (
    <section className="page view-enter" aria-labelledby="diagnostics-title">
      <header className="page-header">
        <div><h1 id="diagnostics-title">{t("nav.diagnostics")}</h1><p>{t("diagnostics.subtitle")}</p></div>
        <button aria-busy={operation === "export"} className="button primary" disabled={operation !== null} onClick={() => void run("export")} type="button">{operation === "export" ? <LoaderCircle aria-hidden="true" className="spin" size={17} /> : <Download aria-hidden="true" size={17} />} {t("diagnostics.export")}</button>
      </header>

      <section className="section-block" aria-labelledby="installation-title">
        <div className="section-heading installation-heading">
          <div><h2 id="installation-title">{t("overview.installation")}</h2><code>{status.root ?? t("overview.noRoot")}</code></div>
          <div className="header-actions">
            <button aria-busy={operation === "relocate"} className="button secondary" disabled={operation !== null} onClick={() => void run("relocate")} type="button">{operation === "relocate" ? <LoaderCircle aria-hidden="true" className="spin" size={17} /> : <FolderSearch aria-hidden="true" size={17} />}{t("overview.relocate")}</button>
            <button aria-busy={operation === "reconnect"} className="button secondary" disabled={operation !== null} onClick={() => void run("reconnect")} type="button">{operation === "reconnect" && <LoaderCircle aria-hidden="true" className="spin" size={17} />}{t("overview.reconnect")}</button>
          </div>
        </div>
        <dl className="detail-list">
          {status.findings.map((finding) => <div key={finding.label}><dt>{findingLabel(finding.label, finding.value)}</dt><dd>{finding.ok ? <Check className="success" aria-label={t("overview.checked")} size={17} /> : <AlertTriangle className="warning" aria-label={t("overview.attention")} size={17} />}<span>{finding.value === "waiting" ? t("finding.waiting") : finding.value === "connected" ? t("overview.checked") : finding.value}</span></dd></div>)}
        </dl>
      </section>

      <section className="section-block" aria-labelledby="components-title">
        <div className="section-heading"><h2 id="components-title">{t("diagnostics.components")}</h2><button aria-busy={operation === "copy"} className="icon-button" disabled={operation !== null} aria-label={t("diagnostics.copy")} onClick={() => void run("copy")} type="button">{operation === "copy" ? <LoaderCircle aria-hidden="true" className="spin" size={17} /> : <Copy aria-hidden="true" size={17} />}</button></div>
        <dl className="detail-list diagnostic-list">
          <div><dt>Control Center</dt><dd><Check className="success" size={17} /><code>0.1.0</code></dd></div>
          <div><dt>{t("diagnostics.core")}</dt><dd><Check className="success" size={17} /><code>{status.coreVersion ?? t("diagnostics.unknown")}</code></dd></div>
        </dl>
      </section>

      <section className="section-block" aria-labelledby="log-title">
        <div className="section-heading"><div><h2 id="log-title">{t("diagnostics.logs")}</h2><p>{t("diagnostics.logsDescription")}</p></div></div>
        {logsExpanded && <div className="log-view" id="diagnostic-log-view" role="log" aria-label={t("diagnostics.logAria")}>
          {operationLogs.length === 0
            ? <div><time>{t("diagnostics.now")}</time><span>{t("diagnostics.logs")}</span><p>{t("diagnostics.noOperationLogs")}</p></div>
            : operationLogs.slice(-20).map((entry, index) => <div key={`${index}-${entry}`}><time>{t("diagnostics.recent")}</time><span>{t("diagnostics.logs")}</span><p>{entry}</p></div>)}
        </div>}
        <div className="disclosure-actions" data-log-disclosure-actions>
          <button aria-busy={operation === "folder"} className="text-action" disabled={operation !== null} onClick={() => void run("folder")} type="button">{operation === "folder" ? <LoaderCircle aria-hidden="true" className="spin" size={15} /> : <ExternalLink aria-hidden="true" size={15} />}{t("diagnostics.openFolder")}</button>
          <button aria-controls="diagnostic-log-view" aria-expanded={logsExpanded} className="text-action" onClick={() => setLogsExpanded((value) => !value)} type="button">{logsExpanded ? t("common.collapse") : t("common.expand")}{logsExpanded ? <ChevronUp aria-hidden="true" size={15} /> : <ChevronDown aria-hidden="true" size={15} />}</button>
        </div>
      </section>
      <div aria-live="polite">
        {completed && <p className="success-message" data-operation={completed.kind}><Check aria-hidden="true" size={16} /> <span>{completed.detail}</span></p>}
        {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
      </div>
    </section>
  );
}
