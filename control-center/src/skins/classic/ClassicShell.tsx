import { Moon, Sun } from "lucide-react";
import { useMemo } from "react";
import { isNavSelected, navGroupLabelKey, viewLabelKey, type ShellProps } from "../../app/shell";
import { LanguagePicker } from "../../components/LanguagePicker";
import { SkinPicker } from "../../components/SkinPicker";
import { WindowTitleBar } from "../../components/WindowTitleBar";
import { findingLabel, findingValue } from "../../features/diagnostics/useDiagnosticsModel";
import { useI18n } from "../../i18n/i18n";
import { ClassicDiagnostics } from "./ClassicDiagnostics";
import { ClassicExecution } from "./ClassicExecution";
import { ClassicFiles } from "./ClassicFiles";
import { ClassicOverview } from "./ClassicOverview";
import { ClassicTuner } from "./ClassicTuner";

/* The classic skin: a labelled navigation pane with grouped entries, one
   work area, and a status bar that stays hidden. This is the layout the
   Control Center shipped with, kept as the default. */
export function ClassicShell(props: ShellProps) {
  const { t } = useI18n();
  const { view, profileMode, navigate, entries } = props.navigation;
  const { status } = props.installation;

  const page = useMemo(() => {
    if (view === "files") return <ClassicFiles onEditInTuner={props.operations.editInTuner} />;
    if (view === "profiles") return <ClassicTuner ciSmoke={props.operations.ciSmoke} mode={profileMode} onModeChange={(mode) => navigate("profiles", mode)} onOpenStudio={props.operations.openPreviewStudio} onPreviewReady={() => props.operations.reportReady("profiles")} />;
    if (view === "execution") return <ClassicExecution ciSmoke={props.operations.ciSmoke} onReady={() => props.operations.reportReady("execution")} />;
    if (view === "diagnostics") return <ClassicDiagnostics
      status={status}
      onReconnect={props.installation.reconnectPreview}
      onRelocate={props.installation.rediscoverInstallation}
    />;
    return <ClassicOverview onOpenService={() => navigate("execution")} />;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [view, profileMode, status, props.operations.ciSmoke]);

  const groups = [null, "wizardGroup", "tunerGroup", "toolsGroup"] as const;

  return (
    <>
      <WindowTitleBar />
      <div className="app-shell" data-testid="app-shell">
        <aside className="navigation" aria-label={t("app.mainMenu")}>
          <div className="product-lockup">
            <img src="/mactype-icon.png" alt="" width="32" height="32" />
            <div>
              <strong>MacType</strong>
              <span>Control Center</span>
            </div>
          </div>
          <nav>
            {groups.map((group) => {
              const groupEntries = entries.filter((entry) => entry.group === group);
              const buttons = groupEntries.map((entry) => {
                const Icon = entry.icon;
                const sub = group !== null && group !== "toolsGroup";
                return (
                  <button className={sub ? "nav-item nav-subitem" : "nav-item"} data-nav={entry.id} data-selected={isNavSelected(entry, view, profileMode)} key={entry.id} onClick={() => navigate(entry.view, entry.profileMode)} type="button">
                    <Icon aria-hidden="true" size={sub ? 17 : 18} strokeWidth={1.8} />
                    <span>{t(entry.labelKey)}</span>
                  </button>
                );
              });
              if (group === null || group === "toolsGroup") return buttons;
              return (
                <div aria-labelledby={`nav-group-${group}`} className="nav-group" key={group} role="group">
                  <span className="nav-group-label" id={`nav-group-${group}`}>{t(navGroupLabelKey(group))}</span>
                  <div className="nav-group-items">{buttons}</div>
                </div>
              );
            })}
          </nav>
          <div className="navigation-preferences">
            <LanguagePicker />
            <SkinPicker onChange={props.preferences.setSkin} skin={props.preferences.skin} />
            <button
              aria-label={props.preferences.theme === "light" ? t("app.themeDark") : t("app.themeLight")}
              className="theme-toggle"
              onClick={props.preferences.toggleTheme}
              type="button"
            >
              {props.preferences.theme === "light" ? <Moon aria-hidden="true" size={17} /> : <Sun aria-hidden="true" size={17} />}
              <span>{props.preferences.theme === "light" ? t("app.themeDark") : t("app.themeLight")}</span>
            </button>
          </div>
        </aside>
        <main className="work-area" id="main-content" tabIndex={-1}>
          {page}
        </main>
      </div>
      <footer className="app-statusbar" data-testid="app-statusbar">
        <span className="app-statusbar-item">{t(viewLabelKey(view, profileMode))}</span>
        {status.findings.map((finding) => (
          <span className="app-statusbar-item" data-ok={finding.ok} key={finding.label}>{findingLabel(t, finding.label, finding.value)} · {findingValue(t, finding.value)}</span>
        ))}
        <span className="app-statusbar-spacer" />
        {status.coreVersion && <span className="app-statusbar-item"><code>{status.coreVersion}</code></span>}
      </footer>
    </>
  );
}
