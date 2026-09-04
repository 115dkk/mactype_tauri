import { Component, type ErrorInfo, type ReactNode } from "react";
import { skinStorageKey } from "./skinPreference";
import { reportFrontendFailure } from "./tauri";

interface AppErrorBoundaryProps {
  children: ReactNode;
}

interface AppErrorBoundaryState {
  error: Error | null;
}

/* A render error must never leave the reader with a blank window. The
   boundary shows what failed, offers to go back to the overview in the
   default skin (the state most likely to render), and to reload. The copy
   is fixed English plus Korean because the localisation layer itself may
   be what failed. */
export class AppErrorBoundary extends Component<AppErrorBoundaryProps, AppErrorBoundaryState> {
  state: AppErrorBoundaryState = { error: null };

  static getDerivedStateFromError(error: Error): AppErrorBoundaryState {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    void reportFrontendFailure("overview", `${error.message}\n${info.componentStack ?? ""}`).catch(() => undefined);
  }

  private recover = (resetSkin: boolean) => {
    try {
      if (resetSkin) window.localStorage.setItem(skinStorageKey, "classic");
    } catch {
      /* Storage may be unavailable; reloading still helps. */
    }
    const url = new URL(window.location.href);
    url.searchParams.delete("view");
    if (resetSkin) url.searchParams.set("skin", "classic");
    window.location.replace(url.toString());
  };

  render(): ReactNode {
    const { error } = this.state;
    if (!error) return this.props.children;
    return (
      <div className="app-recovery" role="alert">
        <h1>화면을 그리는 중 오류가 났습니다 · The window could not be drawn</h1>
        <p>오류 내용은 진단 보고서에 기록됩니다. 아래 버튼으로 돌아갈 수 있습니다.</p>
        <pre>{error.message}</pre>
        <div className="app-recovery-actions">
          <button className="button primary" onClick={() => this.recover(true)} type="button">기본 스킨의 개요로 돌아가기 · Back to the overview in the classic skin</button>
          <button className="button secondary" onClick={() => this.recover(false)} type="button">다시 불러오기 · Reload</button>
        </div>
      </div>
    );
  }
}
