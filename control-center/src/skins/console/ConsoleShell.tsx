import { Moon, Sun } from "lucide-react";
import { useMemo } from "react";
import { isNavSelected, navEntries, type ShellProps } from "../../app/shell";
import { LanguagePicker } from "../../components/LanguagePicker";
import { SkinPicker } from "../../components/SkinPicker";
import { WindowTitleBar } from "../../components/WindowTitleBar";
import { useExecutionModel } from "../../features/execution/useExecutionModel";
import { useI18n } from "../../i18n/i18n";
import { ConsoleContext } from "./consoleContext";
import { ConsoleDiagnostics } from "./ConsoleDiagnostics";
import { ConsoleExecution } from "./ConsoleExecution";
import { ConsoleFiles } from "./ConsoleFiles";
import { ConsoleOverview } from "./ConsoleOverview";
import { ConsoleTuner } from "./ConsoleTuner";

/* The Console skin: a 64-pixel icon rail, a command bar above every page,
   panels with title strips, tabular values in a monospace face, and a
   status bar. The dashboard grammar of a rendering tool's workbench. */
export function ConsoleShell(props: ShellProps) {
  const { t } = useI18n();
  const { view, profileMode, navigate } = props;
  /* One service model for the whole shell: the overview dashboard, the
     service page and every status bar read and act on the same status. */
  const execution = useExecutionModel({
    ciSmoke: props.ciSmoke && view === "execution",
    onReady: () => props.reportReady("execution"),
  });
  const context = useMemo(() => ({ shell: props, execution }), [props, execution]);

  const page = view === "files"
    ? <ConsoleFiles />
    : view === "profiles"
      ? <ConsoleTuner key={profileMode} />
      : view === "execution"
        ? <ConsoleExecution />
        : view === "diagnostics"
          ? <ConsoleDiagnostics />
          : <ConsoleOverview />;

  let previousGroup: string | null | undefined;
  return (
    <ConsoleContext.Provider value={context}>
      <WindowTitleBar />
      <div className="app-shell console-shell" data-testid="app-shell">
        <aside className="navigation console-rail" aria-label={t("app.mainMenu")}>
          <nav className="console-rail-items">
            {navEntries.map((entry) => {
              const Icon = entry.icon;
              const separator = previousGroup !== undefined && previousGroup !== entry.group;
              previousGroup = entry.group;
              return (
                <div key={entry.id}>
                  {separator && <span aria-hidden="true" className="console-rail-sep" />}
                  <button className="nav-item console-rail-item" data-nav={entry.id} data-selected={isNavSelected(entry, view, profileMode)} onClick={() => navigate(entry.view, entry.profileMode)} type="button">
                    <Icon aria-hidden="true" size={20} strokeWidth={1.6} />
                    <span>{t(entry.labelKey)}</span>
                  </button>
                </div>
              );
            })}
          </nav>
          <div className="navigation-preferences console-rail-foot">
            <LanguagePicker />
            <SkinPicker onChange={props.setSkin} skin={props.skin} />
            <button aria-label={props.theme === "light" ? t("app.themeDark") : t("app.themeLight")} className="theme-toggle console-rail-item" onClick={props.toggleTheme} type="button">
              {props.theme === "light" ? <Moon aria-hidden="true" size={18} strokeWidth={1.6} /> : <Sun aria-hidden="true" size={18} strokeWidth={1.6} />}
              <span>{t("app.theme")}</span>
            </button>
          </div>
        </aside>
        <main className="work-area console-work" id="main-content" tabIndex={-1}>
          {page}
        </main>
      </div>
    </ConsoleContext.Provider>
  );
}
