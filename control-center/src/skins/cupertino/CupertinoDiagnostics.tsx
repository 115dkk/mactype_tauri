import { AlertTriangle, Check, LoaderCircle } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { useDiagnosticsModel } from "../../features/diagnostics/useDiagnosticsModel";
import { EventSourceList, EventTimeline } from "../../features/events/EventTimeline";
import { useEventLog } from "../../features/events/useEventLog";
import { useI18n } from "../../i18n/i18n";
import { CupertinoFootnote, CupertinoGroup, CupertinoPage, CupertinoRow, CupertinoSection } from "./CupertinoParts";

export function CupertinoDiagnostics({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
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
  const allOk = model.findings.every((finding) => finding.ok);

  return (
    <CupertinoPage
      actions={<>
        <button aria-busy={operation === "copy"} className="button secondary" disabled={operation !== null} onClick={() => void run("copy")} type="button">{operation === "copy" && <LoaderCircle aria-hidden="true" className="spin" size={14} />}{t("diagnostics.copy")}</button>
        <button aria-busy={operation === "export"} className="button primary" disabled={operation !== null} onClick={() => void run("export")} type="button">{operation === "export" && <LoaderCircle aria-hidden="true" className="spin" size={14} />}{t("diagnostics.export")}…</button>
      </>}
      subtitle={t("diagnostics.subtitle")}
      title={t("nav.diagnostics")}
      titleId="diagnostics-title"
    >
      <CupertinoGroup dataKind="installation">
        <CupertinoRow
          description={<code>{shell.status.root ?? t("overview.noRoot")}</code>}
          hero
          leading={<span className="cupertino-okc" data-tone={allOk ? "ok" : "warn"}>{allOk ? <Check aria-hidden="true" size={16} strokeWidth={3} /> : <AlertTriangle aria-hidden="true" size={15} strokeWidth={2.4} />}</span>}
          title={t("overview.installation")}
          value={<>
            <button aria-busy={operation === "relocate"} className="button secondary" disabled={operation !== null} onClick={() => void run("relocate")} type="button">{operation === "relocate" && <LoaderCircle aria-hidden="true" className="spin" size={14} />}{t("overview.relocate")}</button>
            <button aria-busy={operation === "reconnect"} className="button secondary" disabled={operation !== null} onClick={() => void run("reconnect")} type="button">{operation === "reconnect" && <LoaderCircle aria-hidden="true" className="spin" size={14} />}{t("overview.reconnect")}</button>
          </>}
        />
        {model.findings.map((finding) => (
          <CupertinoRow key={finding.key} title={finding.label} value={<span className="cupertino-value" data-tone={finding.ok ? "ok" : "warn"}>{finding.ok ? <Check aria-hidden="true" size={14} strokeWidth={2.4} /> : <AlertTriangle aria-hidden="true" size={14} strokeWidth={2.2} />} {finding.value}</span>} />
        ))}
      </CupertinoGroup>

      <CupertinoSection title={t("diagnostics.components")}>
        <CupertinoGroup dataKind="components">
          <CupertinoRow title="Control Center" value={<code>0.1.0</code>} />
          <CupertinoRow title={t("diagnostics.core")} value={<code>{shell.status.coreVersion ?? t("diagnostics.unknown")}</code>} />
        </CupertinoGroup>
      </CupertinoSection>

      <CupertinoSection title={t("events.title")}>
        <CupertinoGroup className="cupertino-events" dataKind="events">
          <EventTimeline log={log} />
        </CupertinoGroup>
      </CupertinoSection>

      <CupertinoSection title={t("diagnostics.logs")}>
        <CupertinoGroup dataKind="sources">
          <div className="cupertino-row cupertino-row-block"><EventSourceList log={log} /></div>
          <div className="cupertino-row cupertino-row-link"><button aria-busy={operation === "folder"} className="cupertino-link" disabled={operation !== null} onClick={() => void run("folder")} type="button">{operation === "folder" && <LoaderCircle aria-hidden="true" className="spin" size={14} />}{t("diagnostics.openFolder")}…</button></div>
        </CupertinoGroup>
        <CupertinoFootnote>{t("diagnostics.logsDescription")}</CupertinoFootnote>
      </CupertinoSection>
      <div aria-live="polite">
        {model.completed && <p className="success-message" data-operation={model.completed.kind}><Check aria-hidden="true" size={16} /> <span>{model.completed.detail}</span></p>}
        {model.error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {model.error}</p>}
      </div>
    </CupertinoPage>
  );
}
