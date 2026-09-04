import { Moon, Search, Sun } from "lucide-react";
import { useState } from "react";
import { isNavSelected, navEntries, navGroupLabelKey, viewLabelKey, type NavId, type ShellProps } from "../../app/shell";
import { LanguagePicker } from "../../components/LanguagePicker";
import { SkinPicker } from "../../components/SkinPicker";
import { WindowTitleBar } from "../../components/WindowTitleBar";
import { useI18n } from "../../i18n/i18n";
import { CupertinoDiagnostics } from "./CupertinoDiagnostics";
import { CupertinoExecution } from "./CupertinoExecution";
import { CupertinoFiles } from "./CupertinoFiles";
import { CupertinoOverview } from "./CupertinoOverview";
import { CupertinoTuner } from "./CupertinoTuner";

/* Tile hues are kept muted: one hue family per entry, none of them a
   primary, so the sidebar reads as a list rather than a colour chart. */
const tileTone: Record<NavId, string> = {
  overview: "gray",
  files: "blue",
  execution: "green",
  guided: "violet",
  all: "slate",
  diagnostics: "amber",
};

/* The Cupertino skin: the macOS System Settings grammar. A sidebar that
   runs to the top edge with a search field and tinted icon tiles, a title
   bar named after the page, and rounded groups of rows with the control at
   the trailing edge. */
export function CupertinoShell(props: ShellProps) {
  const { t } = useI18n();
  const { view, profileMode, navigate } = props;
  const [query, setQuery] = useState("");
  const needle = query.trim().toLocaleLowerCase();
  const matches = (labelKey: Parameters<typeof t>[0]) => !needle || t(labelKey).toLocaleLowerCase().includes(needle);
  const groups = [null, "wizardGroup", "tunerGroup", "toolsGroup"] as const;
  const visibleEntries = navEntries.filter((entry) => matches(entry.labelKey));
  const page = view === "files"
    ? <CupertinoFiles shell={props} />
    : view === "profiles"
      ? <CupertinoTuner key={profileMode} shell={props} />
      : view === "execution"
        ? <CupertinoExecution shell={props} />
        : view === "diagnostics"
          ? <CupertinoDiagnostics shell={props} />
          : <CupertinoOverview shell={props} />;

  return (
    <div className="app-shell cupertino-frame" data-testid="app-shell">
      <aside className="navigation cupertino-side" aria-label={t("app.mainMenu")}>
        <div className="cupertino-side-pad" data-tauri-drag-region />
        <label className="cupertino-search"><Search aria-hidden="true" size={13} strokeWidth={2} /><span className="sr-only">{t("nav.search")}</span><input onChange={(event) => setQuery(event.target.value)} placeholder={t("nav.search")} type="search" value={query} /></label>
        <nav className="cupertino-side-items">
          {groups.map((group) => {
            const entries = visibleEntries.filter((entry) => entry.group === group);
            if (entries.length === 0) return null;
            return (
              <div className="cupertino-side-group" key={group ?? "root"} role={group ? "group" : undefined} aria-labelledby={group ? `cupertino-nav-${group}` : undefined}>
                {group && <span className="cupertino-side-head" id={`cupertino-nav-${group}`}>{t(navGroupLabelKey(group))}</span>}
                {entries.map((entry) => {
                  const Icon = entry.icon;
                  return (
                    <button className="nav-item cupertino-item" data-nav={entry.id} data-selected={isNavSelected(entry, view, profileMode)} key={entry.id} onClick={() => navigate(entry.view, entry.profileMode)} type="button">
                      <span className="cupertino-tile" data-tone={tileTone[entry.id]}><Icon aria-hidden="true" size={12} strokeWidth={2.2} /></span>
                      <span>{t(entry.labelKey)}</span>
                    </button>
                  );
                })}
              </div>
            );
          })}
          {visibleEntries.length === 0 && <p className="cupertino-side-empty">{t("nav.noMatches")}</p>}
        </nav>
        <div className="navigation-preferences cupertino-side-foot">
          <LanguagePicker />
          <SkinPicker onChange={props.setSkin} skin={props.skin} />
          <button aria-label={props.theme === "light" ? t("app.themeDark") : t("app.themeLight")} className="theme-toggle cupertino-item cupertino-foot-item" onClick={props.toggleTheme} type="button">
            {props.theme === "light" ? <Moon aria-hidden="true" size={14} strokeWidth={1.8} /> : <Sun aria-hidden="true" size={14} strokeWidth={1.8} />}
            <span>{props.theme === "light" ? t("app.themeDark") : t("app.themeLight")}</span>
          </button>
        </div>
      </aside>
      <div className="cupertino-right">
        <WindowTitleBar className="cupertino-titlebar" icon={false} title={t(viewLabelKey(view, profileMode))} />
        <main className="work-area cupertino-work" id="main-content" tabIndex={-1}>
          {page}
        </main>
      </div>
    </div>
  );
}
