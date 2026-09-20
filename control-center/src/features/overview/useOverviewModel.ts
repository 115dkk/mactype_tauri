import { useEffect, useMemo, useState } from "react";
import { projectExecutionView, type ExecutionViewModel } from "../../app/executionViewModel";
import type { EventRecord, ExecutionStatus } from "../../app/model";
import { runtime } from "../../app/runtimeAdapter";
import { useI18n } from "../../i18n/i18n";
import { eventTitle } from "../events/eventText";

export type OverviewState = ExecutionViewModel["overviewState"];

export function timeText(timestamp: number, locale: string): string {
  return new Intl.DateTimeFormat(locale, { hour: "numeric", minute: "2-digit" }).format(new Date(timestamp));
}

export function clockText(timestamp: number, locale: string): string {
  return new Intl.DateTimeFormat(locale, { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false }).format(new Date(timestamp));
}

interface OverviewModelOptions {
  execution?: ExecutionStatus | null;
  activityLimit?: number;
  liveActivity?: boolean;
}

export function useOverviewModel({ execution: suppliedExecution, activityLimit = 5, liveActivity = false }: OverviewModelOptions = {}) {
  const { locale, t } = useI18n();
  const [loadedExecution, setExecution] = useState<ExecutionStatus | null>(null);
  const [activities, setActivities] = useState<ReadonlyArray<EventRecord>>([]);
  const [expanded, setExpanded] = useState(false);
  const [folderMessage, setFolderMessage] = useState<string | null>(null);

  const hasSuppliedExecution = suppliedExecution !== undefined;
  // The shell snapshot must be available on the first render after navigation.
  const execution = hasSuppliedExecution ? suppliedExecution : loadedExecution;
  const [activityRevision, setActivityRevision] = useState(0);
  const refreshActivities = () => setActivityRevision((revision) => revision + 1);

  useEffect(() => {
    if (hasSuppliedExecution) return;
    let active = true;
    void runtime().loadExecutionStatus().then((nextExecution) => {
      if (active) setExecution(nextExecution);
    }).catch(() => undefined);
    return () => { active = false; };
  }, [hasSuppliedExecution]);

  useEffect(() => {
    let active = true;
    void runtime().loadRecentActivity().then((nextActivities) => {
      if (active) setActivities(nextActivities.slice(-activityLimit));
    }).catch(() => undefined);
    return () => { active = false; };
  }, [activityRevision, activityLimit]);

  useEffect(() => {
    if (liveActivity) return runtime().subscribeEventLog(() => setActivityRevision((revision) => revision + 1));
  }, [liveActivity]);

  const view = useMemo(() => projectExecutionView(execution, null), [execution]);
  const state = view.overviewState;
  const newestFirst = useMemo(() => [...activities].reverse(), [activities]);
  const latestApplied = newestFirst.find((entry) => entry.code === "profile-applied");
  const activityMessage = (entry: EventRecord) => eventTitle(t, locale, entry);
  const openFolder = async () => {
    setFolderMessage(null);
    try {
      setFolderMessage(await runtime().openLogFolder());
    } catch {
      setFolderMessage(t("overview.logFolderFailed"));
    }
  };
  const activeProfile = execution?.activeProfile ?? null;
  const activeProfileName = view.activeProfileDisplay.name;
  const lastAppliedText = latestApplied ? t("overview.todayAt", { time: timeText(latestApplied.ts, locale) }) : t("overview.noLastApplied");

  return {
    activeProfile,
    activeProfileName,
    activities,
    activityMessage,
    execution,
    expanded,
    folderMessage,
    lastAppliedText,
    lastAppliedTimeText: latestApplied ? timeText(latestApplied.ts, locale) : t("overview.noLastApplied"),
    latestApplied,
    locale,
    newestFirst,
    openFolder,
    refreshActivities,
    setExpanded,
    state,
    t,
    view,
  };
}

export type OverviewModel = ReturnType<typeof useOverviewModel>;
