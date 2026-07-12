import { useEffect, useMemo, useReducer } from "react";
import { Activity, FolderCog, Home, Moon, Sun } from "lucide-react";
import { DiagnosticsPage } from "../pages/DiagnosticsPage";
import { OverviewPage } from "../pages/OverviewPage";
import { ProfilesPage } from "../pages/ProfilesPage";
import { fallbackStatus, navigation, type InstallationStatus, type ViewId } from "./model";
import { loadLaunchContext, reportFrontendReady, scanInstallation } from "./tauri";

interface State {
  view: ViewId;
  theme: "light" | "dark";
  status: InstallationStatus;
  ready: boolean;
}

type Action =
  | { type: "navigate"; view: ViewId }
  | { type: "toggle-theme" }
  | { type: "launched"; view: ViewId }
  | { type: "status"; status: InstallationStatus };

function reducer(state: State, action: Action): State {
  switch (action.type) {
    case "navigate":
      return { ...state, view: action.view };
    case "toggle-theme":
      return { ...state, theme: state.theme === "light" ? "dark" : "light" };
    case "launched":
      return { ...state, view: action.view, ready: true };
    case "status":
      return { ...state, status: action.status };
  }
}

const iconByView = { overview: Home, profiles: FolderCog, diagnostics: Activity } as const;

export function App() {
  const [state, dispatch] = useReducer(reducer, {
    view: "overview",
    theme: "light",
    status: fallbackStatus,
    ready: false,
  });

  useEffect(() => {
    let active = true;
    void Promise.all([loadLaunchContext(), scanInstallation()]).then(([context, status]) => {
      if (!active) return;
      dispatch({ type: "launched", view: context.view });
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
    void reportFrontendReady(state.view);
  }, [state.ready, state.theme, state.view]);

  const page = useMemo(() => {
    if (state.view === "profiles") return <ProfilesPage />;
    if (state.view === "diagnostics") return <DiagnosticsPage status={state.status} />;
    return <OverviewPage status={state.status} onOpenProfiles={() => dispatch({ type: "navigate", view: "profiles" })} />;
  }, [state.status, state.view]);

  return (
    <div className="app-shell" data-testid="app-shell">
      <aside className="navigation" aria-label="주 메뉴">
        <div className="product-lockup">
          <img src="/mactype-icon.png" alt="" width="32" height="32" />
          <div>
            <strong>MacType</strong>
            <span>Control Center</span>
          </div>
        </div>
        <nav>
          {navigation.map((item) => {
            const Icon = iconByView[item.id];
            return (
              <button
                className="nav-item"
                data-selected={state.view === item.id}
                key={item.id}
                onClick={() => dispatch({ type: "navigate", view: item.id })}
                type="button"
              >
                <Icon aria-hidden="true" size={18} strokeWidth={1.8} />
                <span>{item.label}</span>
              </button>
            );
          })}
        </nav>
        <button className="theme-toggle" onClick={() => dispatch({ type: "toggle-theme" })} type="button">
          {state.theme === "light" ? <Moon aria-hidden="true" size={17} /> : <Sun aria-hidden="true" size={17} />}
          {state.theme === "light" ? "어두운 테마" : "밝은 테마"}
        </button>
      </aside>
      <main className="work-area" id="main-content" tabIndex={-1}>
        {page}
      </main>
    </div>
  );
}
