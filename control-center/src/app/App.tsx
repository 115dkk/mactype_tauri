import { useEffect, useMemo, useReducer } from "react";
import { Activity, ChevronDown, FileCog, Home, Moon, PlayCircle, Settings2, SlidersHorizontal, Sparkles, Sun } from "lucide-react";
import { DiagnosticsPage } from "../pages/DiagnosticsPage";
import { OverviewPage } from "../pages/OverviewPage";
import { ProfilesPage } from "../pages/ProfilesPage";
import { ExecutionPage } from "../pages/ExecutionPage";
import { FileSettingsPage } from "../pages/FileSettingsPage";
import { fallbackStatus, type InstallationStatus, type ViewId } from "./model";
import { loadLaunchContext, reconnectPreview, rediscoverInstallation, reportFrontendFailure, reportFrontendReady, scanInstallation, verifyTrayModeForCi } from "./tauri";
import { useI18n } from "../i18n/i18n";
import { LanguagePicker } from "../components/LanguagePicker";
import { WindowTitleBar } from "../components/WindowTitleBar";
import { applyThemePreference, loadThemePreference, type ThemePreference } from "./themePreference";

interface State {
  view: ViewId;
  profileMode: ProfileMode;
  settingsExpanded: boolean;
  theme: ThemePreference;
  status: InstallationStatus;
  ready: boolean;
  ciSmoke: boolean;
  trayStart: boolean;
}

type ProfileMode = "quick" | "advanced";

type Action =
  | { type: "navigate"; view: ViewId; profileMode?: ProfileMode }
  | { type: "toggle-settings" }
  | { type: "toggle-theme" }
  | { type: "launched"; view: ViewId; ciSmoke: boolean; trayStart: boolean }
  | { type: "status"; status: InstallationStatus };

function reducer(state: State, action: Action): State {
  switch (action.type) {
    case "navigate":
      return {
        ...state,
        view: action.view,
        profileMode: action.profileMode ?? state.profileMode,
        settingsExpanded: action.view === "files" || action.view === "profiles" ? true : state.settingsExpanded,
      };
    case "toggle-settings":
      return { ...state, settingsExpanded: !state.settingsExpanded };
    case "toggle-theme":
      return { ...state, theme: state.theme === "light" ? "dark" : "light" };
    case "launched":
      return { ...state, view: action.view, ciSmoke: action.ciSmoke, trayStart: action.trayStart, ready: true };
    case "status":
      return { ...state, status: action.status };
  }
}

const primaryNavigation = [
  { view: "overview", icon: Home },
  { view: "execution", icon: PlayCircle },
  { view: "diagnostics", icon: Activity },
] as const;

interface AppProps {
  initialTheme?: ThemePreference;
}

export function App({ initialTheme = loadThemePreference() }: AppProps) {
  const { t } = useI18n();
  const [state, dispatch] = useReducer(reducer, {
    view: "overview",
    profileMode: "advanced",
    settingsExpanded: true,
    theme: initialTheme,
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
    applyThemePreference(state.theme);
  }, [state.theme]);

  useEffect(() => {
    if (!state.ready) return;
    document.body.dataset.view = state.view;
    document.body.dataset.profileMode = state.profileMode;
    document.body.dataset.rendered = "true";
    if (state.ciSmoke && state.trayStart) {
      void verifyTrayModeForCi()
        .then(() => reportFrontendReady(state.view))
        .catch((error: unknown) => reportFrontendFailure(state.view, error instanceof Error ? error.message : String(error)));
    } else if (!state.ciSmoke || (state.view !== "profiles" && state.view !== "execution")) {
      void reportFrontendReady(state.view);
    }
  }, [state.ciSmoke, state.profileMode, state.ready, state.trayStart, state.view]);

  const page = useMemo(() => {
    if (state.view === "files") return <FileSettingsPage />;
    if (state.view === "profiles") return <ProfilesPage ciSmoke={state.ciSmoke} mode={state.profileMode} onModeChange={(profileMode) => dispatch({ type: "navigate", view: "profiles", profileMode })} onPreviewReady={() => void reportFrontendReady("profiles")} />;
    if (state.view === "execution") return <ExecutionPage ciSmoke={state.ciSmoke} onReady={() => void reportFrontendReady("execution")} />;
    if (state.view === "diagnostics") return <DiagnosticsPage status={state.status} />;
    return <OverviewPage
      status={state.status}
      onOpenProfiles={() => dispatch({ type: "navigate", view: "files" })}
      onReconnect={async () => {
        const status = await reconnectPreview();
        dispatch({ type: "status", status });
        return status;
      }}
      onRelocate={async () => {
        const status = await rediscoverInstallation();
        dispatch({ type: "status", status });
        return status;
      }}
    />;
  }, [state.ciSmoke, state.profileMode, state.status, state.view]);

  const settingsSelected = state.view === "files" || state.view === "profiles";

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
          {primaryNavigation.map(({ view, icon: Icon }, index) => (
            <span className="primary-nav-slot" key={view}>
              {index === 1 && (
                <div className="nav-group">
                  <button
                    aria-controls="settings-navigation"
                    aria-expanded={state.settingsExpanded}
                    className="nav-item nav-group-toggle"
                    data-selected={settingsSelected}
                    onClick={() => dispatch({ type: "toggle-settings" })}
                    type="button"
                  >
                    <Settings2 aria-hidden="true" size={18} strokeWidth={1.8} />
                    <span>{t("nav.settings")}</span>
                    <ChevronDown aria-hidden="true" className="nav-group-chevron" size={16} />
                  </button>
                  {state.settingsExpanded && (
                    <div className="nav-submenu" id="settings-navigation">
                      <button className="nav-item nav-subitem" data-selected={state.view === "files"} onClick={() => dispatch({ type: "navigate", view: "files" })} type="button">
                        <FileCog aria-hidden="true" size={17} strokeWidth={1.8} />
                        <span>{t("nav.files")}</span>
                      </button>
                      <button aria-label={`${t("nav.quickSetup")} (Wizard)`} className="nav-item nav-subitem" data-selected={state.view === "profiles" && state.profileMode === "quick"} onClick={() => dispatch({ type: "navigate", view: "profiles", profileMode: "quick" })} type="button">
                        <Sparkles aria-hidden="true" size={17} strokeWidth={1.8} />
                        <span>{t("nav.quickSetup")}<small>Wizard</small></span>
                      </button>
                      <button aria-label={`${t("nav.advancedTuning")} (Tuner)`} className="nav-item nav-subitem" data-selected={state.view === "profiles" && state.profileMode === "advanced"} onClick={() => dispatch({ type: "navigate", view: "profiles", profileMode: "advanced" })} type="button">
                        <SlidersHorizontal aria-hidden="true" size={17} strokeWidth={1.8} />
                        <span>{t("nav.advancedTuning")}<small>Tuner</small></span>
                      </button>
                    </div>
                  )}
                </div>
              )}
              <button
                className="nav-item"
                data-selected={state.view === view}
                onClick={() => dispatch({ type: "navigate", view })}
                type="button"
              >
                <Icon aria-hidden="true" size={18} strokeWidth={1.8} />
                <span>{t(`nav.${view}`)}</span>
              </button>
            </span>
          ))}
        </nav>
        <div className="navigation-preferences">
          <LanguagePicker />
          <button
            aria-label={state.theme === "light" ? t("app.themeDark") : t("app.themeLight")}
            className="theme-toggle"
            onClick={() => dispatch({ type: "toggle-theme" })}
            type="button"
          >
            {state.theme === "light" ? <Moon aria-hidden="true" size={17} /> : <Sun aria-hidden="true" size={17} />}
            <span>{state.theme === "light" ? t("app.themeDark") : t("app.themeLight")}</span>
          </button>
        </div>
      </aside>
      <main className="work-area" id="main-content" tabIndex={-1}>
        {page}
      </main>
      </div>
    </>
  );
}
