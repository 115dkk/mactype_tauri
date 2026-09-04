import { AlertTriangle, Check, CircleCheck, Copy, Download, ExternalLink, FolderSearch, HardDrive, LoaderCircle, Package } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { useDiagnosticsModel } from "../../features/diagnostics/useDiagnosticsModel";
import { EventSourceList, EventTimeline } from "../../features/events/EventTimeline";
import { useEventLog } from "../../features/events/useEventLog";
import { useI18n } from "../../i18n/i18n";
import { FluentCard, FluentCards, FluentPage, FluentSection, FluentState } from "./FluentParts";

export function FluentDiagnostics({ shell }: { shell: ShellProps }) {
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
    <FluentPage
      actions={<>
        <button aria-busy={operation === "copy"} className="button secondary" disabled={operation !== null} onClick={() => void run("copy")} type="button">{operation === "copy" ? <LoaderCircle aria-hidden="true" className="spin" size={16} /> : <Copy aria-hidden="true" size={16} strokeWidth={1.6} />} {t("diagnostics.copy")}</button>
        <button aria-busy={operation === "export"} className="button primary" disabled={operation !== null} onClick={() => void run("export")} type="button">{operation === "export" ? <LoaderCircle aria-hidden="true" className="spin" size={16} /> : <Download aria-hidden="true" size={16} strokeWidth={1.6} />} {t("diagnostics.export")}</button>
      </>}
      subtitle={t("diagnostics.subtitle")}
      title={t("nav.diagnostics")}
      titleId="diagnostics-title"
    >
      <FluentCards>
        <FluentCard
          action={<>
            <button aria-busy={operation === "relocate"} className="button secondary" disabled={operation !== null} onClick={() => void run("relocate")} type="button">{operation === "relocate" ? <LoaderCircle aria-hidden="true" className="spin" size={16} /> : <FolderSearch aria-hidden="true" size={16} strokeWidth={1.6} />} {t("overview.relocate")}</button>
            <button aria-busy={operation === "reconnect"} className="button secondary" disabled={operation !== null} onClick={() => void run("reconnect")} type="button">{operation === "reconnect" && <LoaderCircle aria-hidden="true" className="spin" size={16} />}{t("overview.reconnect")}</button>
          </>}
          dataKind="hero"
          description={<code>{shell.status.root ?? t("overview.noRoot")}</code>}
          hero
          icon={allOk ? <CircleCheck aria-hidden="true" size={28} strokeWidth={1.6} /> : <AlertTriangle aria-hidden="true" size={28} strokeWidth={1.6} />}
          title={t("overview.installation")}
          tone={allOk ? "normal" : "attention"}
        />
        {model.findings.map((finding) => (
          <FluentCard action={<FluentState tone={finding.ok ? "ok" : "warn"}>{finding.ok ? <Check aria-hidden="true" size={16} /> : <AlertTriangle aria-hidden="true" size={16} />} {finding.value}</FluentState>} icon={<HardDrive aria-hidden="true" size={20} strokeWidth={1.6} />} key={finding.key} title={finding.label} />
        ))}
      </FluentCards>

      <FluentSection title={t("diagnostics.components")}>
        <FluentCards>
          <FluentCard action={<FluentState><code>0.1.0</code></FluentState>} icon={<Package aria-hidden="true" size={20} strokeWidth={1.6} />} title="Control Center" />
          <FluentCard action={<FluentState><code>{shell.status.coreVersion ?? t("diagnostics.unknown")}</code></FluentState>} icon={<Package aria-hidden="true" size={20} strokeWidth={1.6} />} title={t("diagnostics.core")} />
        </FluentCards>
      </FluentSection>

      <FluentSection hint={t("diagnostics.logsDescription")} title={t("events.title")}>
        <div className="fluent-card fluent-timeline-card">
          <EventTimeline log={log} />
        </div>
        <div className="fluent-card fluent-sources-card">
          <EventSourceList log={log} />
          <button aria-busy={operation === "folder"} className="text-action fluent-link" disabled={operation !== null} onClick={() => void run("folder")} type="button">{operation === "folder" ? <LoaderCircle aria-hidden="true" className="spin" size={15} /> : <ExternalLink aria-hidden="true" size={15} />} {t("diagnostics.openFolder")}</button>
        </div>
      </FluentSection>
      <div aria-live="polite">
        {model.completed && <p className="success-message" data-operation={model.completed.kind}><Check aria-hidden="true" size={16} /> <span>{model.completed.detail}</span></p>}
        {model.error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {model.error}</p>}
      </div>
    </FluentPage>
  );
}
