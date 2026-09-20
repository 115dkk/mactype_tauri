import { AppWindow, Contrast, ExternalLink, Eye, Pencil, RefreshCw } from "lucide-react";
import { Segmented } from "../../components/Segmented";
import { StatusDot } from "../../components/StatusDot";
import { useAppliedProfileEntry } from "../../features/files/useAppliedProfileEntry";
import { clockText, useOverviewModel } from "../../features/overview/useOverviewModel";
import { useOverviewSpecimen } from "../../features/preview/useOverviewSpecimen";
import { SpecimenBoard } from "../../features/preview/SpecimenBoard";
import { ConsoleFrame, ConsoleKv, ConsolePanel } from "./ConsoleFrame";
import { useConsole } from "./consoleContext";
import { ConsoleServiceStatus } from "./ConsoleStatus";
import { serviceTone } from "./serviceTone";

const SPECIMEN_SIZES = [28, 20, 14, 12, 11, 10] as const;

export function ConsoleOverview() {
  const { shell, execution } = useConsole();
  const model = useOverviewModel({ execution: execution.service.status, activityLimit: Infinity, liveActivity: true });
  const { locale, t, activities, newestFirst, latestApplied, lastAppliedText, lastAppliedTimeText, openFolder, folderMessage } = model;
  const activeProfileName = model.activeProfileName ?? t("execution.profileNotApplied");
  const applied = useAppliedProfileEntry(model.activeProfile);
  const specimen = useOverviewSpecimen({ locale, appliedProfilePath: applied?.path ?? null, expectedProfileDigest: model.execution?.expectedProfileDigest });
  const { fontOptions, selectedFont, fontFace, setFontSource, inverted, setInverted, dark, sample, setSample, editing, setEditing } = specimen;
  const preview = shell.installation.status.findings.find((finding) => finding.label === "preview");
  const helperConnected = preview?.value === "connected";
  const tone = serviceTone(execution.service.serviceSummary.tone);
  const refresh = () => {
    void execution.service.refresh();
    model.refreshActivities();
  };

  return (
    <ConsoleFrame
      actions={<>
        <button className="button secondary" onClick={refresh} type="button"><RefreshCw aria-hidden="true" size={14} /> {t("execution.refresh")}</button>
        {execution.service.serviceSummary.actions.map((action) => (
          <button className={`button ${action.tone === "primary" ? "primary" : "secondary"}${action.tone === "danger" ? " danger" : ""}`} disabled={!action.enabled} key={action.command} onClick={() => execution.service.runSummaryAction(action.command)} type="button">{execution.service.serviceBusy === action.command ? t("execution.serviceWorking") : t(action.labelKey)}</button>
        ))}
      </>}
      bodyClassName="console-cols-main-side"
      status={<ConsoleServiceStatus />}
      statusRight={<span className="app-statusbar-item">Control Center 0.1.0{shell.installation.status.coreVersion && <> · {t("diagnostics.core")} <code>{shell.installation.status.coreVersion}</code></>}</span>}
      summary={<>{t(execution.service.serviceSummary.modeKey)} · <code>{activeProfileName}</code>{latestApplied && <> · {lastAppliedText}</>}</>}
      title={t("nav.overview")}
      titleId="overview-title"
    >
      <ConsolePanel
        className="console-specimen-panel"
        footer={<>
          <span className="console-muted">{t("overview.specimenSource", { profile: applied?.name ?? activeProfileName })}</span>
          <span className="console-spacer" />
          <button aria-expanded={editing} className="button ghost" onClick={() => setEditing((value) => !value)} type="button"><Pencil aria-hidden="true" size={13} /> {t("profiles.editSample")}</button>
          <button className="button ghost" onClick={shell.operations.openPreviewStudio} type="button"><AppWindow aria-hidden="true" size={13} /> {t("profiles.openStudio")}</button>
        </>}
        icon={<Eye aria-hidden="true" size={14} />}
        right={<>
          <Segmented compact label={t("profiles.previewFont")} onChange={setFontSource} options={fontOptions} value={selectedFont.value} />
          <button aria-pressed={inverted} className="button ghost" onClick={() => setInverted((value) => !value)} type="button"><Contrast aria-hidden="true" size={13} /> {t("profiles.invertColours")}</button>
        </>}
        scroll={false}
        title={t("overview.specimenTitle")}
      >
        {editing && <textarea aria-label={t("profiles.sampleAria")} className="sample-input console-sample-input" onChange={(event) => setSample(event.target.value)} rows={2} value={sample} />}
        <SpecimenBoard className="specimen-board console-canvas" dark={dark} fontFace={fontFace} profilePath={specimen.ready ? applied?.path ?? null : null} revision={model.execution?.expectedProfileDigest ?? undefined} sizes={SPECIMEN_SIZES} text={sample} />
        {specimen.error && <p className="inline-error" role="alert">{specimen.error}</p>}
      </ConsolePanel>

      <div className="console-stack">
        <ConsolePanel
          footer={<>
            <button className="button secondary" onClick={() => shell.operations.editInTuner()} type="button">{t("files.editInTuner")}</button>
            <span className="console-spacer" />
            <button className={`button ${execution.service.systemInjectionAction.intent === "stop" ? "secondary" : "primary"}`} disabled={!execution.service.systemInjectionAction.enabled} onClick={() => void execution.service.manageService(execution.service.systemInjectionAction.command)} type="button">{t(execution.service.systemInjectionAction.labelKey)}</button>
          </>}
          scroll={false}
          title={t("nav.execution")}
        >
          <div className="console-big" data-tone={tone}>
            <StatusDot tone={tone} />
            {t(execution.service.serviceSummary.statusKey)}
            <small>{execution.service.serviceStateText}</small>
          </div>
          <ConsoleKv rows={[
            { key: "profile", label: t("execution.summaryProfile"), value: <><code>{activeProfileName}</code>{execution.service.systemInjectionAction.state === "active" && <span className="console-tag">{t("files.inUseBadge")}</span>}</> },
            { key: "mode", label: t("overview.executionMode"), value: t(execution.service.serviceSummary.modeKey) },
            { key: "applied", label: t("overview.lastApplied"), value: lastAppliedTimeText },
            { key: "preview", label: t("finding.preview"), value: <><StatusDot tone={helperConnected ? "ok" : "warn"} /> {helperConnected ? `${t("overview.checked")} · x86` : t("finding.waiting")}</> },
            { key: "core", label: t("diagnostics.core"), value: <code>{shell.installation.status.coreVersion ?? t("diagnostics.unknown")}</code> },
          ]} />
        </ConsolePanel>

        <ConsolePanel
          right={<button className="button ghost" onClick={() => void openFolder()} type="button"><ExternalLink aria-hidden="true" size={12} /> {t("diagnostics.openFolder")}</button>}
          title={t("overview.recentActivity")}
        >
          <ol className="console-log" data-recent-activity>
            {activities.length === 0 && <li className="console-muted">{t("overview.noActivity")}</li>}
            {newestFirst.map((event) => (
              <li data-severity={event.severity} key={`${event.source}:${event.ts}:${event.code}`}><time dateTime={new Date(event.ts).toISOString()}>{clockText(event.ts, locale)}</time>{model.activityMessage(event)}</li>
            ))}
          </ol>
          {folderMessage && <p className="console-muted activity-folder-message" aria-live="polite">{folderMessage}</p>}
        </ConsolePanel>
      </div>
    </ConsoleFrame>
  );
}
