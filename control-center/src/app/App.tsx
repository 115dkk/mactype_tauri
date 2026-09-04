import { useCallback, useEffect, useReducer, type ReactElement } from "react";
import { fallbackStatus, type InstallationStatus, type ViewId } from "./model";
import type { ProfileMode, ShellProps } from "./shell";
import { loadLaunchContext, openPreviewStudio, reconnectPreview, rediscoverInstallation, reportFrontendFailure, reportFrontendReady, scanInstallation, verifyTrayModeForCi } from "./tauri";
import { applySkinPreference, loadSkinPreference, type SkinPreference } from "./skinPreference";
import { applyThemePreference, loadThemePreference, type ThemePreference } from "./themePreference";
import { ClassicShell } from "../skins/classic/ClassicShell";
import { ConsoleShell } from "../skins/console/ConsoleShell";
import { CupertinoShell } from "../skins/cupertino/CupertinoShell";
import { FluentShell } from "../skins/fluent/FluentShell";

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

const shells: Record<SkinPreference, (props: ShellProps) => ReactElement> = {
  classic: ClassicShell,
  fluent: FluentShell,
  console: ConsoleShell,
  cupertino: CupertinoShell,
};

/* The application owns navigation, preferences and the installation status;
   the active skin's shell owns how they are arranged. Switching skins swaps
   the shell and keeps every piece of state. */
export function App({ initialTheme = loadThemePreference(), initialSkin = loadSkinPreference() }: AppProps) {
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

  const navigate = useCallback((view: ViewId, profileMode?: ProfileMode) => dispatch({ type: "navigate", view, profileMode }), []);
  const setStatus = useCallback((status: InstallationStatus) => dispatch({ type: "status", status }), []);
  const reportReady = useCallback((view: ViewId) => { void reportFrontendReady(view); }, []);
  const studio = useCallback(() => { void openPreviewStudio(); }, []);

  const Shell = shells[state.skin];
  return (
    <Shell
      ciSmoke={state.ciSmoke}
      navigate={navigate}
      openPreviewStudio={studio}
      profileMode={state.profileMode}
      reconnectPreview={reconnectPreview}
      rediscoverInstallation={rediscoverInstallation}
      reportReady={reportReady}
      setSkin={(skin) => dispatch({ type: "skin", skin })}
      setStatus={setStatus}
      skin={state.skin}
      status={state.status}
      theme={state.theme}
      toggleTheme={() => dispatch({ type: "toggle-theme" })}
      view={state.view}
    />
  );
}
