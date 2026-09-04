import { AlertTriangle, Check, Copy, Download, ExternalLink, FolderSearch, LoaderCircle } from "lucide-react";
import { StatusDot } from "../../components/StatusDot";
import { useDiagnosticsModel } from "../../features/diagnostics/useDiagnosticsModel";
import { EventFilters, EventSourceList, EventTimeline } from "../../features/events/EventTimeline";
import { useEventLog } from "../../features/events/useEventLog";
import { useI18n } from "../../i18n/i18n";
import { ConsoleFrame, ConsoleKv, ConsolePanel } from "./ConsoleFrame";
import { useConsole } from "./consoleContext";
import { ConsoleServiceStatus } from "./ConsoleStatus";

export function ConsoleDiagnostics() {
  const { t } = useI18n();
  const { shell } = useConsole();
  const model = useDiagnosticsModel({
    status: shell.status,
    onReconnect: async () => {
      const next = await shell.reconnectPreview();
      shell.setStatus(next);
      return next;
    },
    onRelocate: async () => {
      const next = await shell.rediscoverInstallation();
      shell.setStatus(next);
      return next;
    },
  });
  const log = useEventLog();
  const { operation, run } = model;

  return (
    <ConsoleFrame
      actions={<>
        <button aria-busy={operation === "copy"} className="button secondary" disabled={operation !== null} onClick={() => void run("copy")} type="button">{operation === "copy" ? <LoaderCircle aria-hidden="true" className="spin" size={14} /> : <Copy aria-hidden="true" size={14} />} {t("diagnostics.copy")}</button>
        <button aria-busy={operation === "export"} className="button primary" disabled={operation !== null} onClick={() => void run("export")} type="button">{operation === "export" ? <LoaderCircle aria-hidden="true" className="spin" size={14} /> : <Download aria-hidden="true" size={14} />} {t("diagnostics.export")}</button>
      </>}
      bodyClassName="console-cols-side-main-wide"
      crumb={t("nav.toolsGroup")}
      status={<ConsoleServiceStatus />}
      statusRight={log.summary && <span className="app-statusbar-item">{t("event.severity.warning")} {log.summary.warnings} · {t("event.severity.error")} {log.summary.errors}</span>}
      summary={<code>{shell.status.root ?? t("overview.noRoot")}</code>}
      title={t("nav.diagnostics")}
      titleId="diagnostics-title"
    >
      <div className="console-stack">
        <ConsolePanel
          footer={<>
            <button aria-busy={operation === "relocate"} className="button secondary" disabled={operation !== null} onClick={() => void run("relocate")} type="button">{operation === "relocate" ? <LoaderCircle aria-hidden="true" className="spin" size={14} /> : <FolderSearch aria-hidden="true" size={14} />} {t("overview.relocate")}</button>
            <button aria-busy={operation === "reconnect"} className="button secondary" disabled={operation !== null} onClick={() => void run("reconnect")} type="button">{operation === "reconnect" && <LoaderCircle aria-hidden="true" className="spin" size={14} />}{t("overview.reconnect")}</button>
          </>}
          scroll={false}
          title={t("overview.installation")}
        >
          <ConsoleKv rows={model.findings.map((finding) => ({ key: finding.key, label: finding.label, value: <><StatusDot label={finding.ok ? t("overview.checked") : t("overview.attention")} tone={finding.ok ? "ok" : "warn"} /> {finding.value}</> }))} />
        </ConsolePanel>
        <ConsolePanel scroll={false} title={t("diagnostics.components")}>
          <ConsoleKv rows={[
            { key: "cc", label: "Control Center", value: <code>0.1.0</code> },
            { key: "core", label: t("diagnostics.core"), value: <code>{shell.status.coreVersion ?? t("diagnostics.unknown")}</code> },
          ]} />
        </ConsolePanel>
        <ConsolePanel
          footer={<>
            <button aria-busy={operation === "folder"} className="button ghost" disabled={operation !== null} onClick={() => void run("folder")} type="button">{operation === "folder" ? <LoaderCircle aria-hidden="true" className="spin" size={14} /> : <ExternalLink aria-hidden="true" size={14} />} {t("diagnostics.openFolder")}</button>
            <span className="console-spacer" />
            <span className="console-muted">{t("diagnostics.logsDescription")}</span>
          </>}
          title={t("diagnostics.logs")}
        >
          <EventSourceList log={log} />
          <div aria-live="polite">
            {model.completed && <p className="success-message" data-operation={model.completed.kind}><Check aria-hidden="true" size={14} /> <span>{model.completed.detail}</span></p>}
            {model.error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={14} /> {model.error}</p>}
          </div>
        </ConsolePanel>
      </div>
      <ConsolePanel className="console-events-panel" right={<span className="console-muted">{log.visible.length} / {log.events.length}</span>} title={t("events.title")}>
        <EventFilters log={log} />
        <EventTimeline dense filters={false} log={log} />
      </ConsolePanel>
    </ConsoleFrame>
  );
}
