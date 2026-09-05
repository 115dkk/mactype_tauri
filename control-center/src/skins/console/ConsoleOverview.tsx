import { AppWindow, Contrast, ExternalLink, Eye, Pencil, RefreshCw } from "lucide-react";
import { useState } from "react";
import { openLogFolder } from "../../app/tauri";
import { useAppTheme } from "../../app/useAppTheme";
import { Segmented } from "../../components/Segmented";
import { StatusDot } from "../../components/StatusDot";
import { eventClock, eventTime, eventTitle } from "../../features/events/eventText";
import { useRecentActivity } from "../../features/events/useEventLog";
import { useAppliedProfileEntry } from "../../features/files/useAppliedProfileEntry";
import { scriptUiFont } from "../../features/preview/scriptUiFont";
import { previewFontOptions } from "../../features/preview/previewFonts";
import { usePreviewFontSubstitutes } from "../../features/preview/usePreviewFontSubstitutes";
import { SpecimenBoard } from "../../features/preview/SpecimenBoard";
import { useI18n } from "../../i18n/i18n";
import { ConsoleFrame, ConsoleKv, ConsolePanel } from "./ConsoleFrame";
import { useConsole } from "./consoleContext";
import { ConsoleServiceStatus } from "./ConsoleStatus";
import { serviceTone } from "./serviceTone";

const SPECIMEN_SIZES = [28, 20, 14, 12, 11, 10] as const;

export function ConsoleOverview() {
  const { locale, t } = useI18n();
  const { shell, execution } = useConsole();
  const activity = useRecentActivity();
  const applied = useAppliedProfileEntry(execution.status?.activeProfile ?? null);
  const scriptFont = scriptUiFont(locale);
  const substitutes = usePreviewFontSubstitutes(applied?.path ?? null, execution.status?.expectedProfileDigest);
  const fontOptions = previewFontOptions(locale, substitutes.mappings);
  const [fontSource, setFontSource] = useState<string | null>(null);
  const selectedFont = fontOptions.find((option) => option.value === (fontSource ?? scriptFont)) ?? fontOptions[0];
  const fontFace = selectedFont.label;
  const theme = useAppTheme();
  const [inverted, setInverted] = useState(false);
  const dark = (theme === "dark") !== inverted;
  const [sample, setSample] = useState(() => t("profiles.samplePangram"));
  const [editing, setEditing] = useState(false);
  const [folderMessage, setFolderMessage] = useState<string | null>(null);

  const latestApplied = [...activity.events].reverse().find((event) => event.code === "profile-applied");
  const preview = shell.status.findings.find((finding) => finding.label === "preview");
  const helperConnected = preview?.value === "connected";
  const tone = serviceTone(execution.serviceSummary.tone);
  const refresh = () => {
    void execution.refresh();
    activity.refresh();
  };
  const openFolder = async () => {
    try {
      setFolderMessage(await openLogFolder());
    } catch {
      setFolderMessage(t("overview.logFolderFailed"));
    }
  };

  return (
    <ConsoleFrame
      actions={<>
        <button className="button secondary" onClick={refresh} type="button"><RefreshCw aria-hidden="true" size={14} /> {t("execution.refresh")}</button>
        {execution.serviceSummary.actions.map((action) => (
          <button className={`button ${action.tone === "primary" ? "primary" : "secondary"}${action.tone === "danger" ? " danger" : ""}`} disabled={!action.enabled} key={action.command} onClick={() => execution.runSummaryAction(action.command)} type="button">{execution.serviceBusy === action.command ? t("execution.serviceWorking") : t(action.labelKey)}</button>
        ))}
      </>}
      bodyClassName="console-cols-main-side"
      status={<ConsoleServiceStatus />}
      statusRight={<span className="app-statusbar-item">Control Center 0.1.0{shell.status.coreVersion && <> · {t("diagnostics.core")} <code>{shell.status.coreVersion}</code></>}</span>}
      summary={<>{t(execution.serviceSummary.modeKey)} · <code>{execution.activeProfileName}</code>{latestApplied && <> · {t("overview.todayAt", { time: eventClock(latestApplied.ts, locale) })}</>}</>}
      title={t("nav.overview")}
      titleId="overview-title"
    >
      <ConsolePanel
        className="console-specimen-panel"
        footer={<>
          <span className="console-muted">{t("overview.specimenSource", { profile: applied?.name ?? execution.activeProfileName })}</span>
          <span className="console-spacer" />
          <button aria-expanded={editing} className="button ghost" onClick={() => setEditing((value) => !value)} type="button"><Pencil aria-hidden="true" size={13} /> {t("profiles.editSample")}</button>
          <button className="button ghost" onClick={shell.openPreviewStudio} type="button"><AppWindow aria-hidden="true" size={13} /> {t("profiles.openStudio")}</button>
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
        <SpecimenBoard className="specimen-board console-canvas" dark={dark} fontFace={fontFace} profilePath={applied?.path ?? null} sizes={SPECIMEN_SIZES} text={sample} />
        {substitutes.error && <p className="inline-error" role="alert">{substitutes.error}</p>}
      </ConsolePanel>

      <div className="console-stack">
        <ConsolePanel
          footer={<>
            <button className="button secondary" onClick={() => shell.navigate("profiles", "advanced")} type="button">{t("files.editInTuner")}</button>
            <span className="console-spacer" />
            <button className={`button ${execution.systemInjectionAction.intent === "stop" ? "secondary" : "primary"}`} disabled={!execution.systemInjectionAction.enabled} onClick={() => void execution.manageService(execution.systemInjectionAction.command)} type="button">{t(execution.systemInjectionAction.labelKey)}</button>
          </>}
          scroll={false}
          title={t("nav.execution")}
        >
          <div className="console-big" data-tone={tone}>
            <StatusDot tone={tone} />
            {t(execution.serviceSummary.statusKey)}
            <small>{execution.serviceStateText}</small>
          </div>
          <ConsoleKv rows={[
            { key: "profile", label: t("execution.summaryProfile"), value: <><code>{execution.activeProfileName}</code>{execution.systemInjectionAction.state === "active" && <span className="console-tag">{t("files.appliedBadge")}</span>}</> },
            { key: "mode", label: t("overview.executionMode"), value: t(execution.serviceSummary.modeKey) },
            { key: "applied", label: t("overview.lastApplied"), value: latestApplied ? eventClock(latestApplied.ts, locale) : t("overview.noLastApplied") },
            { key: "preview", label: t("finding.preview"), value: <><StatusDot tone={helperConnected ? "ok" : "warn"} /> {helperConnected ? `${t("overview.checked")} · x86` : t("finding.waiting")}</> },
            { key: "core", label: t("diagnostics.core"), value: <code>{shell.status.coreVersion ?? t("diagnostics.unknown")}</code> },
          ]} />
        </ConsolePanel>

        <ConsolePanel
          right={<button className="button ghost" onClick={() => void openFolder()} type="button"><ExternalLink aria-hidden="true" size={12} /> {t("diagnostics.openFolder")}</button>}
          title={t("overview.recentActivity")}
        >
          <ol className="console-log" data-recent-activity>
            {activity.events.length === 0 && <li className="console-muted">{t("overview.noActivity")}</li>}
            {[...activity.events].reverse().map((event) => (
              <li data-severity={event.severity} key={`${event.source}:${event.ts}:${event.code}`}><time dateTime={new Date(event.ts).toISOString()}>{eventTime(event.ts, locale)}</time>{eventTitle(t, locale, event)}</li>
            ))}
          </ol>
          {folderMessage && <p className="console-muted activity-folder-message" aria-live="polite">{folderMessage}</p>}
        </ConsolePanel>
      </div>
    </ConsoleFrame>
  );
}
