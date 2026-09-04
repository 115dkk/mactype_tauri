import { Moon, Sun } from "lucide-react";
import { isNavSelected, navEntries, navGroupLabelKey, type ShellProps } from "../../app/shell";
import { LanguagePicker } from "../../components/LanguagePicker";
import { SkinPicker } from "../../components/SkinPicker";
import { WindowTitleBar } from "../../components/WindowTitleBar";
import { useI18n } from "../../i18n/i18n";
import { FluentDiagnostics } from "./FluentDiagnostics";
import { FluentExecution } from "./FluentExecution";
import { FluentFiles } from "./FluentFiles";
import { FluentOverview } from "./FluentOverview";
import { FluentTuner } from "./FluentTuner";

/* The Fluent skin: the Windows 11 Settings grammar. One Mica surface, a
   navigation pane with section headings, a 28-pixel page title, and
   settings cards (icon, title, description, control) instead of tables. */
export function FluentShell(props: ShellProps) {
  const { t } = useI18n();
  const { view, profileMode, navigate } = props;
  const page = view === "files"
    ? <FluentFiles shell={props} />
    : view === "profiles"
      ? <FluentTuner key={profileMode} shell={props} />
      : view === "execution"
        ? <FluentExecution shell={props} />
        : view === "diagnostics"
          ? <FluentDiagnostics shell={props} />
          : <FluentOverview shell={props} />;
  const groups = [null, "wizardGroup", "tunerGroup", "toolsGroup"] as const;

  return (
    <>
      <WindowTitleBar />
      <div className="app-shell fluent-shell" data-testid="app-shell">
        <aside className="navigation fluent-nav" aria-label={t("app.mainMenu")}>
          <nav className="fluent-nav-items">
            {groups.map((group) => {
              const entries = navEntries.filter((entry) => entry.group === group);
              return (
                <div className="fluent-nav-group" key={group ?? "root"} role={group ? "group" : undefined} aria-labelledby={group ? `fluent-nav-${group}` : undefined}>
                  {group && <span className="fluent-nav-head" id={`fluent-nav-${group}`}>{t(navGroupLabelKey(group))}</span>}
                  {entries.map((entry) => {
                    const Icon = entry.icon;
                    return (
                      <button className="nav-item fluent-nav-item" data-nav={entry.id} data-selected={isNavSelected(entry, view, profileMode)} key={entry.id} onClick={() => navigate(entry.view, entry.profileMode)} type="button">
                        <Icon aria-hidden="true" size={16} strokeWidth={1.6} />
                        <span>{t(entry.labelKey)}</span>
                      </button>
                    );
                  })}
                </div>
              );
            })}
          </nav>
          <div className="navigation-preferences fluent-nav-foot">
            <LanguagePicker />
            <SkinPicker onChange={props.setSkin} skin={props.skin} />
            <button aria-label={props.theme === "light" ? t("app.themeDark") : t("app.themeLight")} className="theme-toggle fluent-nav-item" onClick={props.toggleTheme} type="button">
              {props.theme === "light" ? <Moon aria-hidden="true" size={16} strokeWidth={1.6} /> : <Sun aria-hidden="true" size={16} strokeWidth={1.6} />}
              <span>{props.theme === "light" ? t("app.themeDark") : t("app.themeLight")}</span>
            </button>
          </div>
        </aside>
        <main className="work-area fluent-work" id="main-content" tabIndex={-1}>
          {page}
        </main>
      </div>
    </>
  );
}
