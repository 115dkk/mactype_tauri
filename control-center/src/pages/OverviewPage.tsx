import { AlertTriangle, Check, ChevronDown, ChevronUp, ExternalLink, Power, ServerCog } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { projectExecutionView } from "../app/executionViewModel";
import type { RecentActivity } from "../app/model";
import { loadExecutionStatus, loadRecentActivity, openLogFolder } from "../app/tauri";
import { useI18n, type MessageKey } from "../i18n/i18n";

interface OverviewPageProps {
  onOpenService: () => void;
}

function timeText(timestamp: number, locale: string): string {
  return new Intl.DateTimeFormat(locale, { hour: "numeric", minute: "2-digit" }).format(new Date(timestamp));
}

export function OverviewPage({ onOpenService }: OverviewPageProps) {
  const { locale, t } = useI18n();
  const [execution, setExecution] = useState<Awaited<ReturnType<typeof loadExecutionStatus>> | null>(null);
  const [activities, setActivities] = useState<ReadonlyArray<RecentActivity>>([]);
  const [expanded, setExpanded] = useState(false);
  const [folderMessage, setFolderMessage] = useState<string | null>(null);

  useEffect(() => {
    let active = true;
    void loadExecutionStatus().then((nextExecution) => {
      if (active) setExecution(nextExecution);
    }).catch(() => undefined);
    void loadRecentActivity().then((nextActivities) => {
      if (active) setActivities(nextActivities.slice(-5));
    }).catch(() => undefined);
    return () => { active = false; };
  }, []);

  const view = useMemo(() => projectExecutionView(execution, null), [execution]);
  const state = view.systemInjectionAction.state === "active"
    ? "normal"
    : execution?.systemService.runtime === "stopped"
      ? "inactive"
      : "problem";
  const newestFirst = [...activities].reverse();
  const latestApplied = newestFirst.find((entry) => entry.activity === "profile-applied");
  const activityMessage = (entry: RecentActivity) => t(
    `overview.activity.${entry.activity}` as MessageKey,
    { profile: entry.profile ?? t("overview.unknownProfile") },
  );
  const openFolder = async () => {
    setFolderMessage(null);
    try {
      setFolderMessage(await openLogFolder());
    } catch {
      setFolderMessage(t("overview.logFolderFailed"));
    }
  };

  return (
    <section className="page view-enter" aria-labelledby="overview-title">
      <header className="page-header compact">
        <div><h1 id="overview-title">{t("nav.overview")}</h1><p>{t("overview.subtitle")}</p></div>
      </header>

      <section className="overview-service-card section-block" data-overview-service data-state={state} aria-labelledby="overview-service-title">
        <div className="overview-service-heading">
          {state === "normal" ? <Check aria-hidden="true" size={22} /> : state === "inactive" ? <Power aria-hidden="true" size={22} /> : <AlertTriangle aria-hidden="true" size={22} />}
          <h2 id="overview-service-title">{t(`overview.${state}Title` as MessageKey)}</h2>
          {state !== "normal" && <button className="button secondary" onClick={onOpenService} type="button"><ServerCog aria-hidden="true" size={17} />{t("nav.execution")}</button>}
        </div>
        <dl className="overview-service-details">
          <div><dt>{t("overview.activeProfile")}</dt><dd><code>{execution?.activeProfile ?? t("overview.unknownProfile")}</code></dd></div>
          <div><dt>{t("overview.executionMode")}</dt><dd>{t(view.serviceSummary.modeKey)}</dd></div>
          <div><dt>{t("overview.status")}</dt><dd>{t(`overview.${state}` as MessageKey)}</dd></div>
          <div><dt>{t("overview.lastApplied")}</dt><dd>{latestApplied ? t("overview.todayAt", { time: timeText(latestApplied.timestampUnixMs, locale) }) : t("overview.noLastApplied")}</dd></div>
        </dl>
      </section>

      <section className="section-block recent-activity" data-recent-activity aria-labelledby="recent-activity-title">
        <div className="section-heading"><h2 id="recent-activity-title">{t("overview.recentActivity")}</h2></div>
        <div className="activity-latest">
          {newestFirst[0]
            ? <p>{activityMessage(newestFirst[0])}<span aria-hidden="true"> · </span><time dateTime={new Date(newestFirst[0].timestampUnixMs).toISOString()}>{timeText(newestFirst[0].timestampUnixMs, locale)}</time></p>
            : <p>{t("overview.noActivity")}</p>}
        </div>
        {expanded && newestFirst.length > 0 && (
          <ol id="overview-activity-list">
            {newestFirst.map((entry, index) => <li key={`${entry.timestampUnixMs}-${index}`}><time dateTime={new Date(entry.timestampUnixMs).toISOString()}>{timeText(entry.timestampUnixMs, locale)}</time><span>{activityMessage(entry)}</span></li>)}
          </ol>
        )}
        <div className="disclosure-actions">
          <button className="text-action" onClick={() => void openFolder()} type="button"><ExternalLink aria-hidden="true" size={15} />{t("diagnostics.openFolder")}</button>
          <button aria-controls="overview-activity-list" aria-expanded={expanded} className="text-action" onClick={() => setExpanded((value) => !value)} type="button">{expanded ? t("common.collapse") : t("common.expand")}{expanded ? <ChevronUp aria-hidden="true" size={15} /> : <ChevronDown aria-hidden="true" size={15} />}</button>
        </div>
        {folderMessage && <p className="activity-folder-message" aria-live="polite">{folderMessage}</p>}
      </section>
    </section>
  );
}
