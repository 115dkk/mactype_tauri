import { Activity, AlertTriangle, CircleCheck, ExternalLink, FileText, Monitor, Power, ServerCog } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { eventClock, eventTitle } from "../../features/events/eventText";
import { useOverviewModel } from "../../features/overview/useOverviewModel";
import { useI18n, type MessageKey } from "../../i18n/i18n";
import { FluentCard, FluentCards, FluentPage, FluentSection, FluentState, FluentSubRow } from "./FluentParts";

export function FluentOverview({ shell }: { shell: ShellProps }) {
  const { locale, t } = useI18n();
  const model = useOverviewModel();
  const { state, newestFirst, latestApplied, view } = model;
  const preview = shell.status.findings.find((finding) => finding.label === "preview");
  const helperConnected = preview?.value === "connected";
  const profileName = model.activeProfileName ?? t("overview.unknownProfile");
  const modeText = t(view.serviceSummary.modeKey);
  const heroDescription = state === "normal"
    ? `${t("overview.heroRunning", { mode: modeText, profile: profileName })}${latestApplied ? ` ${t("overview.lastAppliedAt", { time: t("overview.todayAt", { time: eventClock(latestApplied.ts, locale) }) })}` : ""}`
    : t(`overview.${state}` as MessageKey);
  const earlier = newestFirst.slice(1);

  return (
    <FluentPage subtitle={t("overview.subtitle")} title={t("nav.overview")} titleId="overview-title">
      <FluentCards>
        <FluentCard
          action={<button className="button secondary" onClick={() => shell.navigate("execution")} type="button">{t("overview.manageService")}</button>}
          dataKind="hero"
          description={heroDescription}
          hero
          icon={state === "normal" ? <CircleCheck aria-hidden="true" size={28} strokeWidth={1.6} /> : state === "inactive" ? <Power aria-hidden="true" size={28} strokeWidth={1.6} /> : <AlertTriangle aria-hidden="true" size={28} strokeWidth={1.6} />}
          title={t(`overview.${state}Title` as MessageKey)}
          tone={state === "normal" ? "normal" : state === "inactive" ? "neutral" : "attention"}
        />
        <FluentCard
          action={<><FluentState><code>{model.activeProfile ?? t("overview.unknownProfile")}</code></FluentState><button className="button secondary" onClick={() => shell.navigate("profiles", "advanced")} type="button">{t("files.editInTuner")}</button></>}
          description={t("overview.activeProfileHint")}
          icon={<FileText aria-hidden="true" size={20} strokeWidth={1.6} />}
          title={t("overview.activeProfile")}
        />
        <FluentCard
          action={<FluentState>{modeText}</FluentState>}
          description={t("overview.executionModeHint")}
          icon={<ServerCog aria-hidden="true" size={20} strokeWidth={1.6} />}
          title={t("overview.executionMode")}
        />
        <FluentCard
          action={<FluentState tone={helperConnected ? "ok" : "warn"}>{helperConnected ? t("overview.checked") : t("finding.waiting")}</FluentState>}
          description={helperConnected ? t("overview.previewHint") : t("overview.previewNeededDescription")}
          icon={<Monitor aria-hidden="true" size={20} strokeWidth={1.6} />}
          title={t("finding.preview")}
        />
      </FluentCards>

      <FluentSection title={t("overview.recentActivity")}>
        <FluentCards>
          <div data-recent-activity>
            <FluentCard
              action={<button className="text-action fluent-link" onClick={() => void model.openFolder()} type="button"><ExternalLink aria-hidden="true" size={14} /> {t("diagnostics.openFolder")}</button>}
              description={newestFirst[0] ? `${t("overview.todayAt", { time: eventClock(newestFirst[0].ts, locale) })}${earlier.length ? ` · ${t("overview.earlierActivities", { count: earlier.length })}` : ""}` : undefined}
              expanded={model.expanded}
              icon={<Activity aria-hidden="true" size={20} strokeWidth={1.6} />}
              onToggle={earlier.length ? () => model.setExpanded((value) => !value) : undefined}
              title={newestFirst[0] ? eventTitle(t, locale, newestFirst[0]) : t("overview.noActivity")}
            >
              <ol className="fluent-activity-list" id="overview-activity-list">
                {earlier.map((entry) => <li key={`${entry.source}:${entry.ts}:${entry.code}`}><FluentSubRow description={eventClock(entry.ts, locale)} title={eventTitle(t, locale, entry)} /></li>)}
              </ol>
            </FluentCard>
            {model.folderMessage && <p className="activity-folder-message fluent-sub" aria-live="polite">{model.folderMessage}</p>}
          </div>
        </FluentCards>
      </FluentSection>
    </FluentPage>
  );
}
