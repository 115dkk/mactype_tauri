import { AlertTriangle, Check, Power } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { eventClock, eventTitle } from "../../features/events/eventText";
import { useOverviewModel } from "../../features/overview/useOverviewModel";
import { useI18n, type MessageKey } from "../../i18n/i18n";
import { CupertinoBadge, CupertinoGroup, CupertinoPage, CupertinoRow, CupertinoSection } from "./CupertinoParts";

export function CupertinoOverview({ shell }: { shell: ShellProps }) {
  const { locale, t } = useI18n();
  const model = useOverviewModel();
  const { state, newestFirst, latestApplied, view } = model;
  const preview = shell.status.findings.find((finding) => finding.label === "preview");
  const helperConnected = preview?.value === "connected";
  const profileName = model.activeProfileName ?? t("overview.unknownProfile");
  const modeText = t(view.serviceSummary.modeKey);
  const shown = model.expanded ? newestFirst : newestFirst.slice(0, 2);
  const hidden = newestFirst.length - shown.length;

  return (
    <CupertinoPage subtitle={t("overview.subtitle")} title={t("nav.overview")} titleId="overview-title">
      <CupertinoGroup dataKind="service">
        <CupertinoRow
          dataKind="hero"
          description={state === "normal" ? t("overview.heroRunning", { mode: modeText, profile: profileName }) : t(`overview.${state}` as MessageKey)}
          hero
          leading={<span className="cupertino-okc" data-tone={state === "normal" ? "ok" : state === "inactive" ? "neutral" : "warn"}>{state === "normal" ? <Check aria-hidden="true" size={16} strokeWidth={3} /> : state === "inactive" ? <Power aria-hidden="true" size={15} strokeWidth={2.4} /> : <AlertTriangle aria-hidden="true" size={15} strokeWidth={2.4} />}</span>}
          title={t(`overview.${state}Title` as MessageKey)}
          value={<button className="button secondary" onClick={() => shell.navigate("execution")} type="button">{t("overview.manageService")}…</button>}
        />
        <CupertinoRow onDisclose={() => shell.navigate("files")} title={t("overview.activeProfile")} value={<><code>{model.activeProfile ?? t("overview.unknownProfile")}</code>{state === "normal" && <CupertinoBadge>{t("files.appliedBadge")}</CupertinoBadge>}</>} />
        <CupertinoRow title={t("overview.executionMode")} value={modeText} />
        <CupertinoRow onDisclose={() => shell.navigate("diagnostics")} title={t("finding.preview")} value={<span className="cupertino-value" data-tone={helperConnected ? "ok" : "warn"}>{helperConnected ? t("overview.checked") : t("finding.waiting")}</span>} />
        <CupertinoRow title={t("overview.lastApplied")} value={latestApplied ? t("overview.todayAt", { time: eventClock(latestApplied.ts, locale) }) : t("overview.noLastApplied")} />
      </CupertinoGroup>

      <CupertinoSection title={t("overview.recentActivity")}>
        <div data-recent-activity>
          <CupertinoGroup dataKind="activity">
            {shown.length === 0 && <CupertinoRow title={t("overview.noActivity")} />}
            {shown.map((entry, index) => (
              <CupertinoRow
                description={t("overview.todayAt", { time: eventClock(entry.ts, locale) })}
                key={`${entry.source}:${entry.ts}:${entry.code}`}
                title={eventTitle(t, locale, entry)}
                value={index === 0 ? <button className="button secondary" onClick={() => void model.openFolder()} type="button">{t("diagnostics.openFolder")}…</button> : undefined}
              />
            ))}
            {hidden > 0 && <div className="cupertino-row cupertino-row-link"><button className="cupertino-link" onClick={() => model.setExpanded(true)} type="button">{t("overview.showEarlierActivities", { count: hidden })}</button></div>}
            {model.expanded && newestFirst.length > 2 && <div className="cupertino-row cupertino-row-link"><button className="cupertino-link" onClick={() => model.setExpanded(false)} type="button">{t("common.collapse")}</button></div>}
          </CupertinoGroup>
          {model.folderMessage && <p className="cupertino-footnote activity-folder-message" aria-live="polite">{model.folderMessage}</p>}
        </div>
      </CupertinoSection>
    </CupertinoPage>
  );
}
