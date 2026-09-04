import { ChevronDown, Search, X } from "lucide-react";
import type { EventRecord } from "../../app/model";
import { useI18n } from "../../i18n/i18n";
import { areaLabel, eventTime, eventTitle, groupEventsByDay, severityLabel, sourceLabel } from "./eventText";
import { eventAreas, eventSeverities, type EventLogModel } from "./useEventLog";

interface EventTimelineProps {
  log: EventLogModel;
  /* Skins that draw the filter chips in their own toolbar pass false. */
  filters?: boolean;
  /* Dense layout drops the day headings and shows one line per event. */
  dense?: boolean;
}

/* The event timeline every skin shows on the diagnostics page: newest day
   first, one localised line per event, severity as a filled dot, technical
   detail only behind a disclosure. Raw backend text is never the headline. */
export function EventTimeline({ log, filters = true, dense = false }: EventTimelineProps) {
  const { locale, t } = useI18n();
  const groups = groupEventsByDay(log.visible, locale);

  return (
    <div className="event-timeline" data-dense={dense} data-testid="event-timeline">
      {filters && <EventFilters log={log} />}
      {log.error && <p className="inline-error">{log.error}</p>}
      {!log.loading && log.visible.length === 0 && (
        <p className="event-empty">{log.filtered ? t("events.noMatches") : t("events.empty")}</p>
      )}
      {groups.map((group) => (
        <section className="event-day" key={group.day}>
          {!dense && <h3 className="event-day-heading">{group.day}</h3>}
          <ol className="event-list">
            {group.events.map((event) => <EventRow event={event} key={log.eventKey(event)} log={log} />)}
          </ol>
        </section>
      ))}
    </div>
  );
}

function EventRow({ event, log }: { event: EventRecord; log: EventLogModel }) {
  const { locale, t } = useI18n();
  const key = log.eventKey(event);
  const open = log.expanded === key;
  const detailId = `event-detail-${event.ts}-${event.code}`;
  const technical = Object.entries(event.params).map(([name, value]) => `${name}=${value}`).join("  ");
  return (
    <li className="event-row" data-area={event.area} data-severity={event.severity} data-source={event.source}>
      <span aria-label={severityLabel(t, event.severity)} className="event-dot" role="img" />
      <time className="event-time" dateTime={new Date(event.ts).toISOString()}>{eventTime(event.ts, locale)}</time>
      <span className="event-area">{areaLabel(t, event.area)}</span>
      <p className="event-title">{eventTitle(t, locale, event)}</p>
      <button aria-controls={detailId} aria-expanded={open} aria-label={t("events.detail")} className="event-disclosure" onClick={() => log.setExpanded(open ? null : key)} type="button"><ChevronDown aria-hidden="true" size={14} /></button>
      {open && (
        <div className="event-detail" id={detailId}>
          <dl>
            <div><dt>{t("events.source")}</dt><dd>{sourceLabel(t, event.source)}</dd></div>
            <div><dt>{t("events.code")}</dt><dd><code>{event.code}</code></dd></div>
            <div><dt>{t("events.severity")}</dt><dd>{severityLabel(t, event.severity)}</dd></div>
            {technical && <div><dt>{t("events.parameters")}</dt><dd><code>{technical}</code></dd></div>}
          </dl>
          {event.detail && <pre className="event-detail-text">{event.detail}</pre>}
        </div>
      )}
    </li>
  );
}

export function EventFilters({ log }: { log: EventLogModel }) {
  const { t } = useI18n();
  return (
    <div className="event-filters" role="group" aria-label={t("events.filters")}>
      <div className="event-chip-row" role="group" aria-label={t("events.severity")}>
        {eventSeverities.map((severity) => (
          <button aria-pressed={log.severities.has(severity)} className="event-chip" data-severity={severity} key={severity} onClick={() => log.toggleSeverity(severity)} type="button"><span className="event-dot" aria-hidden="true" />{severityLabel(t, severity)}</button>
        ))}
      </div>
      <div className="event-chip-row" role="group" aria-label={t("events.area")}>
        {eventAreas.map((area) => (
          <button aria-pressed={log.areas.has(area)} className="event-chip" key={area} onClick={() => log.toggleArea(area)} type="button">{areaLabel(t, area)}</button>
        ))}
      </div>
      <label className="event-search search-field"><Search aria-hidden="true" size={15} /><span className="sr-only">{t("events.search")}</span><input onChange={(event) => log.setQuery(event.target.value)} placeholder={t("events.search")} type="search" value={log.query} /></label>
      {log.filtered && <button className="text-action" onClick={log.resetFilters} type="button"><X aria-hidden="true" size={14} /> {t("events.resetFilters")}</button>}
    </div>
  );
}

export function EventSourceList({ log }: { log: EventLogModel }) {
  const { t } = useI18n();
  if (!log.summary) return null;
  return (
    <dl className="event-sources">
      {log.summary.sources.map((source) => (
        <div data-readable={source.readable} key={source.source}>
          <dt>{sourceLabel(t, source.source)}</dt>
          <dd><code title={source.path}>{source.path}</code><span>{source.readable ? t("events.sourceReadable", { size: Math.round(source.bytes / 1024) }) : t("events.sourceUnreadable")}</span></dd>
        </div>
      ))}
    </dl>
  );
}
