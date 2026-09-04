import { useEffect, useMemo, useReducer } from "react";
import { Activity, FileCog, Home, Moon, ServerCog, SlidersHorizontal, Sparkles, Sun } from "lucide-react";
import { DiagnosticsPage } from "../pages/DiagnosticsPage";
import { OverviewPage } from "../pages/OverviewPage";
import { ProfilesPage } from "../pages/ProfilesPage";
import { ExecutionPage } from "../pages/ExecutionPage";
import { FileSettingsPage } from "../pages/FileSettingsPage";
import { fallbackStatus, type InstallationStatus, type ViewId } from "./model";
import { loadLaunchContext, reconnectPreview, rediscoverInstallation, reportFrontendFailure, reportFrontendReady, scanInstallation, verifyTrayModeForCi } from "./tauri";
import { useI18n } from "../i18n/i18n";
import { LanguagePicker } from "../components/LanguagePicker";
import { SkinPicker } from "../components/SkinPicker";
import { WindowTitleBar } from "../components/WindowTitleBar";
import { applySkinPreference, loadSkinPreference, type SkinPreference } from "./skinPreference";
import { applyThemePreference, loadThemePreference, type ThemePreference } from "./themePreference";

interface State {
  view: ViewId;
  profileMode: ProfileMode;
  theme: ThemePreference;
  skin: SkinPreference;
  status: InstallationStatus;
  ready: boolean;
  ciSmoke: boolean;
  trayStart: boolean;
}

type ProfileMode = "quick" | "advanced";

type Action =
  | { type: "navigate"; view: ViewId; profileMode?: ProfileMode }
  | { type: "toggle-theme" }
  | { type: "skin"; skin: SkinPreference }
  | { type: "launched"; view: ViewId; ciSmoke: boolean; trayStart: boolean }
  | { type: "status"; status: InstallationStatus };

function reducer(state: State, action: Action): State {
  switch (action.type) {
    case "navigate":
      return {
        ...state,
        view: action.view,
        profileMode: action.profileMode ?? state.profileMode,
      };
    case "toggle-theme":
      return { ...state, theme: state.theme === "light" ? "dark" : "light" };
    case "skin":
      return { ...state, skin: action.skin };
    case "launched":
      return { ...state, view: action.view, ciSmoke: action.ciSmoke, trayStart: action.trayStart, ready: true };
    case "status":
      return { ...state, status: action.status };
  }
}

interface AppProps {
  initialTheme?: ThemePreference;
  initialSkin?: SkinPreference;
}

export function App({ initialTheme = loadThemePreference(), initialSkin = loadSkinPreference() }: AppProps) {
  const { t } = useI18n();
  const [state, dispatch] = useReducer(reducer, {
    view: "overview",
    profileMode: "advanced",
    theme: initialTheme,
    skin: initialSkin,
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
    applySkinPreference(state.skin);
  }, [state.skin]);

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
    if (state.view === "files") return <FileSettingsPage onEditInTuner={() => dispatch({ type: "navigate", view: "profiles", profileMode: "advanced" })} />;
    if (state.view === "profiles") return <ProfilesPage ciSmoke={state.ciSmoke} mode={state.profileMode} onModeChange={(profileMode) => dispatch({ type: "navigate", view: "profiles", profileMode })} onPreviewReady={() => void reportFrontendReady("profiles")} />;
    if (state.view === "execution") return <ExecutionPage ciSmoke={state.ciSmoke} onReady={() => void reportFrontendReady("execution")} />;
    if (state.view === "diagnostics") return <DiagnosticsPage
      status={state.status}
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
    return <OverviewPage onOpenService={() => dispatch({ type: "navigate", view: "execution" })} />;
  }, [state.ciSmoke, state.profileMode, state.status, state.view]);

  /* The status bar reads the same installation findings as the diagnostics
     page and names them the same way. */
  const findingLabel = (label: string, value: string) => {
    if (value === "MacType.dll") return t("finding.core32");
    if (value === "MacType64.dll") return t("finding.core64");
    if (value === "MacLoader.exe") return t("finding.loader");
    if (label === "preview") return t("finding.preview");
    return label;
  };
  const findingValue = (value: string) => (value === "waiting" ? t("finding.waiting") : value === "connected" ? t("overview.checked") : value);

  const viewLabel = state.view === "files"
    ? t("nav.profiles")
    : state.view === "profiles"
      ? (state.profileMode === "quick" ? t("nav.guidedSetup") : t("nav.allSettings"))
      : state.view === "execution"
        ? t("nav.execution")
        : state.view === "diagnostics"
          ? t("nav.diagnostics")
          : t("nav.overview");

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
          <button className="nav-item" data-nav="overview" data-selected={state.view === "overview"} onClick={() => dispatch({ type: "navigate", view: "overview" })} type="button">
            <Home aria-hidden="true" size={18} strokeWidth={1.8} />
            <span>{t("nav.overview")}</span>
          </button>
          <div aria-labelledby="nav-group-wizard" className="nav-group" role="group">
            <span className="nav-group-label" id="nav-group-wizard">{t("nav.wizardGroup")}</span>
            <div className="nav-group-items">
              <button className="nav-item nav-subitem" data-nav="files" data-selected={state.view === "files"} onClick={() => dispatch({ type: "navigate", view: "files" })} type="button">
                <FileCog aria-hidden="true" size={17} strokeWidth={1.8} />
                <span>{t("nav.profiles")}</span>
              </button>
              <button className="nav-item nav-subitem" data-nav="execution" data-selected={state.view === "execution"} onClick={() => dispatch({ type: "navigate", view: "execution" })} type="button">
                <ServerCog aria-hidden="true" size={17} strokeWidth={1.8} />
                <span>{t("nav.execution")}</span>
              </button>
            </div>
          </div>
          <div aria-labelledby="nav-group-tuner" className="nav-group" role="group">
            <span className="nav-group-label" id="nav-group-tuner">{t("nav.tunerGroup")}</span>
            <div className="nav-group-items">
              <button className="nav-item nav-subitem" data-nav="guided" data-selected={state.view === "profiles" && state.profileMode === "quick"} onClick={() => dispatch({ type: "navigate", view: "profiles", profileMode: "quick" })} type="button">
                <Sparkles aria-hidden="true" size={17} strokeWidth={1.8} />
                <span>{t("nav.guidedSetup")}</span>
              </button>
              <button className="nav-item nav-subitem" data-nav="all" data-selected={state.view === "profiles" && state.profileMode === "advanced"} onClick={() => dispatch({ type: "navigate", view: "profiles", profileMode: "advanced" })} type="button">
                <SlidersHorizontal aria-hidden="true" size={17} strokeWidth={1.8} />
                <span>{t("nav.allSettings")}</span>
              </button>
            </div>
          </div>
          <button className="nav-item" data-nav="diagnostics" data-selected={state.view === "diagnostics"} onClick={() => dispatch({ type: "navigate", view: "diagnostics" })} type="button">
            <Activity aria-hidden="true" size={18} strokeWidth={1.8} />
            <span>{t("nav.diagnostics")}</span>
          </button>
        </nav>
        <div className="navigation-preferences">
          <LanguagePicker />
          <SkinPicker onChange={(skin) => dispatch({ type: "skin", skin })} skin={state.skin} />
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
      <footer className="app-statusbar" data-testid="app-statusbar">
        <span className="app-statusbar-item">{viewLabel}</span>
        {state.status.findings.map((finding) => (
          <span className="app-statusbar-item" data-ok={finding.ok} key={finding.label}>{findingLabel(finding.label, finding.value)} · {findingValue(finding.value)}</span>
        ))}
        <span className="app-statusbar-spacer" />
        {state.status.coreVersion && <span className="app-statusbar-item"><code>{state.status.coreVersion}</code></span>}
      </footer>
    </>
  );
}
