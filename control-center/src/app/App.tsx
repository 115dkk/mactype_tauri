import { useEffect, useMemo, useReducer } from "react";
import { Activity, FolderCog, Home, Languages, Moon, PlayCircle, Sun } from "lucide-react";
import { DiagnosticsPage } from "../pages/DiagnosticsPage";
import { OverviewPage } from "../pages/OverviewPage";
import { ProfilesPage } from "../pages/ProfilesPage";
import { ExecutionPage } from "../pages/ExecutionPage";
import { fallbackStatus, type InstallationStatus, type ViewId } from "./model";
import { loadLaunchContext, reportFrontendFailure, reportFrontendReady, scanInstallation, verifyTrayModeForCi } from "./tauri";
import { localeOptions, useI18n, type Locale } from "../i18n/i18n";

interface State {
  view: ViewId;
  theme: "light" | "dark";
  status: InstallationStatus;
  ready: boolean;
  ciSmoke: boolean;
  trayStart: boolean;
}

type Action =
  | { type: "navigate"; view: ViewId }
  | { type: "toggle-theme" }
  | { type: "launched"; view: ViewId; ciSmoke: boolean; trayStart: boolean }
  | { type: "status"; status: InstallationStatus };

function reducer(state: State, action: Action): State {
  switch (action.type) {
    case "navigate":
      return { ...state, view: action.view };
    case "toggle-theme":
      return { ...state, theme: state.theme === "light" ? "dark" : "light" };
    case "launched":
      return { ...state, view: action.view, ciSmoke: action.ciSmoke, trayStart: action.trayStart, ready: true };
    case "status":
      return { ...state, status: action.status };
  }
}

const iconByView = { overview: Home, profiles: FolderCog, execution: PlayCircle, diagnostics: Activity } as const;
const navigation: ReadonlyArray<ViewId> = ["overview", "profiles", "execution", "diagnostics"];

export function App() {
  const { locale, setLocale, t } = useI18n();
  const [state, dispatch] = useReducer(reducer, {
    view: "overview",
    theme: "light",
    status: fallbackStatus,
    ready: false,
    ciSmoke: false,
    trayStart: false,
  });

  useEffect(() => {
    let active = true;
    void Promise.all([loadLaunchContext(), scanInstallation()]).then(([context, status]) => {
      if (!active) return;
      dispatch({ type: "launched", view: context.view, ciSmoke: context.ciSmoke, trayStart: context.trayStart });
      if (status) dispatch({ type: "status", status });
    });
    return () => {
      active = false;
    };
  }, []);

  useEffect(() => {
    if (!state.ready) return;
    document.documentElement.dataset.theme = state.theme;
    document.body.dataset.view = state.view;
    document.body.dataset.rendered = "true";
    if (state.ciSmoke && state.trayStart) {
      void verifyTrayModeForCi()
        .then(() => reportFrontendReady(state.view))
        .catch((error: unknown) => reportFrontendFailure(state.view, error instanceof Error ? error.message : String(error)));
    } else if (!state.ciSmoke || (state.view !== "profiles" && state.view !== "execution")) {
      void reportFrontendReady(state.view);
    }
  }, [state.ciSmoke, state.ready, state.theme, state.trayStart, state.view]);

  const page = useMemo(() => {
    if (state.view === "profiles") return <ProfilesPage ciSmoke={state.ciSmoke} onPreviewReady={() => void reportFrontendReady("profiles")} />;
    if (state.view === "execution") return <ExecutionPage ciSmoke={state.ciSmoke} onReady={() => void reportFrontendReady("execution")} />;
    if (state.view === "diagnostics") return <DiagnosticsPage status={state.status} />;
    return <OverviewPage status={state.status} onOpenProfiles={() => dispatch({ type: "navigate", view: "profiles" })} />;
  }, [state.ciSmoke, state.status, state.view]);

  return (
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
          {navigation.map((view) => {
            const Icon = iconByView[view];
            return (
              <button
                className="nav-item"
                data-selected={state.view === view}
                key={view}
                onClick={() => dispatch({ type: "navigate", view })}
                type="button"
              >
                <Icon aria-hidden="true" size={18} strokeWidth={1.8} />
                <span>{t(`nav.${view}`)}</span>
              </button>
            );
          })}
        </nav>
        <div className="navigation-preferences">
          <label className="language-control">
            <Languages aria-hidden="true" size={17} />
            <span className="sr-only">{t("app.language")}</span>
            <select aria-label={t("app.language")} onChange={(event) => setLocale(event.target.value as Locale)} value={locale}>
              {localeOptions.map((option) => (
                <option key={option.value} value={option.value}>
                  {t(option.labelKey)}
                </option>
              ))}
            </select>
          </label>
          <button className="theme-toggle" onClick={() => dispatch({ type: "toggle-theme" })} type="button">
            {state.theme === "light" ? <Moon aria-hidden="true" size={17} /> : <Sun aria-hidden="true" size={17} />}
            <span>{state.theme === "light" ? t("app.themeDark") : t("app.themeLight")}</span>
          </button>
        </div>
      </aside>
      <main className="work-area" id="main-content" tabIndex={-1}>
        {page}
      </main>
    </div>
  );
}
