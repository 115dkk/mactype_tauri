import { FolderOpen, Plus, RefreshCw, Trash2 } from "lucide-react";
import { useCallback, useEffect, useState } from "react";
import type { ManualLaunchCandidate } from "../../app/model";
import { listManualLaunchCandidates, pickExecutable } from "../../app/tauri";
import type { I18nValue } from "../../i18n/i18n";

interface UnityGamePickerProps {
  games: ReadonlyArray<string>;
  onChange: (games: ReadonlyArray<string>) => void;
  t: I18nValue["t"];
}

function executableName(path: string): string {
  return path.split(/[\\/]/).pop() ?? path;
}

const sameName = (left: string, right: string) => left.trim().toLocaleLowerCase() === right.trim().toLocaleLowerCase();

/* One row per executable: several processes of one game are one choice. */
function uniqueCandidates(candidates: ReadonlyArray<ManualLaunchCandidate>): ReadonlyArray<ManualLaunchCandidate> {
  const seen = new Map<string, ManualLaunchCandidate>();
  for (const candidate of candidates) {
    const key = candidate.name.toLocaleLowerCase();
    const existing = seen.get(key);
    if (!existing || (!existing.windowTitle && candidate.windowTitle)) seen.set(key, candidate);
  }
  return [...seen.values()];
}

/* The games that "selected games only" applies to. The profile stores them
   as executable names in its [UnityInclude] section, so the picker offers
   the running apps, a file picker for a game that is not running, and a
   typed name for the rest. */
export function UnityGamePicker({ games, onChange, t }: UnityGamePickerProps) {
  const [candidates, setCandidates] = useState<ReadonlyArray<ManualLaunchCandidate>>([]);
  const [loadingCandidates, setLoadingCandidates] = useState(false);
  const [filter, setFilter] = useState("");
  const [pending, setPending] = useState("");
  const [rejection, setRejection] = useState<string | null>(null);

  const loadCandidates = useCallback(async () => {
    setLoadingCandidates(true);
    try {
      setCandidates(uniqueCandidates(await listManualLaunchCandidates()));
    } catch {
      setCandidates([]);
    } finally {
      setLoadingCandidates(false);
    }
  }, []);

  useEffect(() => {
    let active = true;
    setLoadingCandidates(true);
    void listManualLaunchCandidates()
      .then((loaded) => {
        if (active) setCandidates(uniqueCandidates(loaded));
      })
      .catch(() => {
        if (active) setCandidates([]);
      })
      .finally(() => {
        if (active) setLoadingCandidates(false);
      });
    return () => {
      active = false;
    };
  }, []);

  const selectedEntry = (name: string) => games.find((entry) => sameName(entry, name));
  const add = (name: string) => {
    const value = name.trim();
    if (!value) return;
    if (selectedEntry(value)) {
      setRejection(t("profiles.duplicateListEntry", { name: value }));
      return;
    }
    onChange([...games, value]);
    setRejection(null);
  };
  const remove = (entry: string) => {
    onChange(games.filter((existing) => existing !== entry));
    setRejection(null);
  };
  const toggleCandidate = (candidate: ManualLaunchCandidate) => {
    const existing = selectedEntry(candidate.name);
    if (existing) remove(existing);
    else add(candidate.name);
  };
  const browse = async () => {
    const selected = await pickExecutable(t("execution.executableFilter"));
    if (selected) add(executableName(selected));
  };

  const needle = filter.trim().toLocaleLowerCase();
  const visible = candidates.filter((candidate) => !needle
    || candidate.name.toLocaleLowerCase().includes(needle)
    || (candidate.windowTitle ?? "").toLocaleLowerCase().includes(needle));

  return (
    <section aria-labelledby="unity-games-title" className="unity-games" data-testid="unity-game-picker">
      <div className="unity-games-head">
        <strong id="unity-games-title">{t("list.unityIncludeGames.label")}</strong>
        <p>{t("advanced.unitySelectedHint")}</p>
      </div>
      {games.length > 0
        ? <ul className="unity-games-list">{games.map((game) => <li key={game}><code>{game}</code><button aria-label={t("profiles.remove", { name: game })} className="icon-button" onClick={() => remove(game)} type="button"><Trash2 aria-hidden="true" size={14} /></button></li>)}</ul>
        : <p className="unity-games-empty">{t("advanced.unityNoneSelected")}</p>}
      <div className="process-picker unity-games-picker">
        <div className="process-picker-heading">
          <strong>{t("advanced.unityPickRunning")}</strong>
          <div className="process-picker-tools">
            <input aria-label={t("execution.processListFilter")} className="process-picker-filter" onChange={(event) => setFilter(event.target.value)} placeholder={t("execution.processListFilter")} type="search" value={filter} />
            <button className="button secondary" disabled={loadingCandidates} onClick={() => void loadCandidates()} type="button"><RefreshCw aria-hidden="true" size={16} /> {t("execution.processListRefresh")}</button>
          </div>
        </div>
        {visible.length ? (
          <ul className="process-picker-list">
            {visible.map((candidate) => {
              const selected = Boolean(selectedEntry(candidate.name));
              return (
                <li key={candidate.name.toLocaleLowerCase()}>
                  <label className="process-picker-row">
                    <input checked={selected} onChange={() => toggleCandidate(candidate)} type="checkbox" value={candidate.name} />
                    <strong>{candidate.name}</strong>
                    {candidate.windowTitle && <span className="process-picker-window">{candidate.windowTitle}</span>}
                    <span className="process-picker-pid">{t("execution.processPid", { pid: candidate.pid })}</span>
                  </label>
                </li>
              );
            })}
          </ul>
        ) : (
          <p className="empty-state">{t("execution.processListEmpty")}</p>
        )}
      </div>
      <form className="list-add-row unity-games-add" onSubmit={(event) => { event.preventDefault(); add(pending); setPending(""); }}>
        <input aria-label={t("profiles.addListEntry")} onChange={(event) => { setPending(event.target.value); setRejection(null); }} placeholder={t("profiles.listEntryPlaceholder")} type="text" value={pending} />
        <button className="button" disabled={!pending.trim()} type="submit"><Plus aria-hidden="true" size={14} /> {t("profiles.addListEntry")}</button>
        <button className="button secondary" onClick={() => void browse()} type="button"><FolderOpen aria-hidden="true" size={14} /> {t("advanced.unityBrowseExe")}</button>
      </form>
      {rejection && <p className="inline-error" role="alert">{rejection}</p>}
    </section>
  );
}
