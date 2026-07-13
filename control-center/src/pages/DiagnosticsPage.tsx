import { Check, Copy, Download, ExternalLink } from "lucide-react";
import { useEffect, useState } from "react";
import type { InstallationStatus } from "../app/model";
import { loadPreviewDiagnostics } from "../app/tauri";
import { useI18n } from "../i18n/i18n";

export function DiagnosticsPage({ status }: { status: InstallationStatus }) {
  const { t } = useI18n();
  const [helperLogs, setHelperLogs] = useState<ReadonlyArray<string>>([]);

  useEffect(() => {
    let active = true;
    void loadPreviewDiagnostics().then((entries) => {
      if (active) setHelperLogs(entries);
    });
    return () => {
      active = false;
    };
  }, []);

  return (
    <section className="page view-enter" aria-labelledby="diagnostics-title">
      <header className="page-header">
        <div><h1 id="diagnostics-title">{t("nav.diagnostics")}</h1><p>{t("diagnostics.subtitle")}</p></div>
        <button className="button primary" type="button"><Download aria-hidden="true" size={17} /> {t("diagnostics.export")}</button>
      </header>
      <section className="section-block" aria-labelledby="components-title">
        <div className="section-heading"><h2 id="components-title">{t("diagnostics.components")}</h2><button className="icon-button" aria-label={t("diagnostics.copy")} type="button"><Copy aria-hidden="true" size={17} /></button></div>
        <dl className="detail-list diagnostic-list">
          <div><dt>Control Center</dt><dd><Check className="success" size={17} /><code>0.1.0</code></dd></div>
          <div><dt>{t("diagnostics.core")}</dt><dd><Check className="success" size={17} /><code>{status.coreVersion ?? t("diagnostics.unknown")}</code></dd></div>
          <div><dt>Preview Helper</dt><dd><Check className="success" size={17} /><span>{t("diagnostics.helperState")}</span></dd></div>
          <div><dt>{t("diagnostics.protocol")}</dt><dd><code>MTPC v1</code></dd></div>
        </dl>
      </section>
      <section className="section-block" aria-labelledby="log-title">
        <div className="section-heading"><div><h2 id="log-title">{t("diagnostics.logs")}</h2><p>{t("diagnostics.logsDescription")}</p></div><button className="text-action" type="button">{t("diagnostics.openFolder")} <ExternalLink aria-hidden="true" size={15} /></button></div>
        <div className="log-view" role="log" aria-label={t("diagnostics.logAria")}>
          <div><time>12:18:04</time><span>{t("diagnostics.installScan")}</span><p>{t("diagnostics.installScanMessage")}</p></div>
          <div><time>12:18:04</time><span>{t("diagnostics.fileCheck")}</span><p>{t("diagnostics.fileCheckMessage")}</p></div>
          {helperLogs.length === 0 ? (
            <div><time>{t("diagnostics.now")}</time><span>{t("diagnostics.preview")}</span><p>{t("diagnostics.noHelperError")}</p></div>
          ) : helperLogs.slice(-20).map((entry, index) => (
            <div data-severity="warning" key={`${index}-${entry}`}><time>{t("diagnostics.recent")}</time><span>{t("diagnostics.preview")}</span><p>{entry}</p></div>
          ))}
        </div>
      </section>
    </section>
  );
}
