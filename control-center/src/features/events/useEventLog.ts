import { useCallback, useEffect, useMemo, useState } from "react";
import type { EventArea, EventLogSummary, EventRecord, EventSeverity } from "../../app/model";
import { listEvents, loadEventLogSummary, loadRecentActivity, subscribeEventLog } from "../../app/tauri";

export const eventSeverities: ReadonlyArray<EventSeverity> = ["info", "notice", "warning", "error"];
export const eventAreas: ReadonlyArray<EventArea> = ["service", "setup", "profile", "preview", "injection", "control-center", "tray"];

/* The overview feed: routine and noteworthy events only, newest last,
   refreshed whenever the backend reports new lines. */
export function useRecentActivity() {
  const [events, setEvents] = useState<ReadonlyArray<EventRecord>>([]);
  const [summary, setSummary] = useState<EventLogSummary | null>(null);
  const refresh = useCallback(() => {
    void loadRecentActivity().then(setEvents).catch(() => undefined);
    void loadEventLogSummary().then(setSummary).catch(() => undefined);
  }, []);
  useEffect(() => {
    refresh();
    return subscribeEventLog(refresh);
  }, [refresh]);
  return { events, summary, refresh };
}

export interface EventLogFilters {
  severities: ReadonlySet<EventSeverity>;
  areas: ReadonlySet<EventArea>;
  query: string;
}

const DEFAULT_LIMIT = 300;

/* The diagnostics timeline: every event within the limit, filtered locally so
   toggling a chip never waits on the backend, with live refresh. */
export function useEventLog() {
  const [events, setEvents] = useState<ReadonlyArray<EventRecord>>([]);
  const [summary, setSummary] = useState<EventLogSummary | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [severities, setSeverities] = useState<ReadonlySet<EventSeverity>>(() => new Set(eventSeverities));
  const [areas, setAreas] = useState<ReadonlySet<EventArea>>(() => new Set(eventAreas));
  const [query, setQuery] = useState("");
  const [expanded, setExpanded] = useState<string | null>(null);

  const refresh = useCallback(() => {
    void Promise.all([listEvents(undefined, DEFAULT_LIMIT), loadEventLogSummary()])
      .then(([nextEvents, nextSummary]) => {
        setEvents(nextEvents);
        setSummary(nextSummary);
        setError(null);
      })
      .catch((caught: unknown) => setError(caught instanceof Error ? caught.message : String(caught)))
      .finally(() => setLoading(false));
  }, []);

  useEffect(() => {
    refresh();
    return subscribeEventLog(refresh);
  }, [refresh]);

  const toggleSeverity = (severity: EventSeverity) => setSeverities((current) => {
    const next = new Set(current);
    if (next.has(severity)) next.delete(severity);
    else next.add(severity);
    return next;
  });
  const toggleArea = (area: EventArea) => setAreas((current) => {
    const next = new Set(current);
    if (next.has(area)) next.delete(area);
    else next.add(area);
    return next;
  });
  const resetFilters = () => {
    setSeverities(new Set(eventSeverities));
    setAreas(new Set(eventAreas));
    setQuery("");
  };

  const needle = query.trim().toLocaleLowerCase();
  const visible = useMemo(() => events.filter((event) =>
    severities.has(event.severity)
    && areas.has(event.area)
    && (!needle || `${event.code} ${Object.values(event.params).join(" ")} ${event.detail ?? ""}`.toLocaleLowerCase().includes(needle))), [areas, events, needle, severities]);
  const filtered = visible.length !== events.length || needle.length > 0;
  const eventKey = (event: EventRecord) => `${event.source}:${event.ts}:${event.code}`;

  return {
    areas,
    error,
    eventKey,
    events,
    expanded,
    filtered,
    loading,
    query,
    refresh,
    resetFilters,
    setExpanded,
    setQuery,
    severities,
    summary,
    toggleArea,
    toggleSeverity,
    visible,
  };
}

export type EventLogModel = ReturnType<typeof useEventLog>;
