import { AlertTriangle, Check, Play, RefreshCw, ShieldAlert } from "lucide-react";
import { useCallback, useEffect, useState } from "react";
import type { ExecutionStatus } from "../app/model";
import { launchTargetWithMactype, loadExecutionStatus, setSessionAutostart } from "../app/tauri";

export function ExecutionPage({ ciSmoke = false, onReady }: { ciSmoke?: boolean; onReady?: () => void }) {
  const [status, setStatus] = useState<ExecutionStatus | null>(null);
  const [target, setTarget] = useState("");
  const [argumentsText, setArgumentsText] = useState("");
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setStatus(await loadExecutionStatus());
      setError(null);
      if (ciSmoke) onReady?.();
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  }, [ciSmoke, onReady]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const toggleAutostart = async (enabled: boolean) => {
    try {
      const actual = await setSessionAutostart(enabled);
      setStatus((current) => current ? { ...current, autoStart: actual } : current);
      setMessage(actual ? "로그인할 때 트레이로 시작합니다." : "로그인 자동 시작을 해제했습니다.");
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const launch = async () => {
    try {
      const arguments_ = argumentsText.split(/\r?\n/).map((argument) => argument.trim()).filter(Boolean);
      const pid = await launchTargetWithMactype(target, arguments_);
      setMessage(`MacLoader를 통해 프로세스 ${pid}을(를) 시작했습니다.`);
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  return (
    <section className="page view-enter" aria-labelledby="execution-title">
      <header className="page-header">
        <div><h1 id="execution-title">실행</h1><p>공개 MacLoader를 사용하는 안전한 사용자 세션 모드와 트레이 시작을 관리합니다.</p></div>
        <button className="button secondary" onClick={() => void refresh()} type="button"><RefreshCw aria-hidden="true" size={16} /> 상태 새로 고침</button>
      </header>

      <section className="section-block" aria-labelledby="tray-title">
        <div className="section-heading"><div><h2 id="tray-title">새 Control Center 트레이</h2><p>창을 닫으면 Delphi MacTray가 아니라 이 공개 Tauri 프로그램이 트레이에 남습니다.</p></div></div>
        <div className="execution-option">
          <div>{status?.trayAvailable ? <Check className="success" aria-hidden="true" size={18} /> : <AlertTriangle className="warning" aria-hidden="true" size={18} />}<div><strong>로그인 시 트레이 시작</strong><p>사용자별 HKCU Run 항목만 사용하며 관리자 권한이 필요하지 않습니다.</p></div></div>
          <label className="switch-control"><input checked={status?.autoStart ?? false} disabled={!status} onChange={(event) => void toggleAutostart(event.target.checked)} type="checkbox" /><span>{status?.autoStart ? "사용" : "사용 안 함"}</span></label>
        </div>
      </section>

      <section className="section-block" aria-labelledby="manual-title">
        <div className="section-heading"><div><h2 id="manual-title">수동 실행 모드</h2><p>선택한 프로그램 하나를 기존 공개 MacLoader로 직접 실행합니다. 셸을 사용하지 않습니다.</p></div></div>
        <div className="manual-launcher">
          <label><span>실행 파일의 전체 경로</span><input onChange={(event) => setTarget(event.target.value)} placeholder="C:\\Windows\\System32\\notepad.exe" type="text" value={target} /></label>
          <label><span>인수 — 한 줄에 하나</span><textarea onChange={(event) => setArgumentsText(event.target.value)} placeholder="문서 경로처럼 필요한 인수를 한 줄씩 입력" rows={3} value={argumentsText} /></label>
          <button className="button primary" disabled={!status?.manualLauncherAvailable || !target.trim()} onClick={() => void launch()} type="button"><Play aria-hidden="true" size={17} /> MacType로 실행</button>
        </div>
      </section>

      <section className="section-block" aria-labelledby="system-title">
        <div className="section-heading"><div><h2 id="system-title">시스템 범위 모드</h2><p>현재 시스템의 상태는 감지하지만 위험하거나 비공개 구성 요소를 자동 제어하지 않습니다.</p></div></div>
        <dl className="detail-list">
          <div><dt>기존 MacType 서비스</dt><dd>{status?.legacyServiceDetected ? <AlertTriangle className="warning" size={17} /> : <Check className="success" size={17} />}<span>{status?.legacyServiceDetected ? `감지됨 · ${status.legacyServiceRunning ? "실행 중" : "중지됨"}` : "감지되지 않음"}</span></dd></div>
          <div><dt>AppInit 레지스트리 모드</dt><dd>{status?.registryModeDetected ? <ShieldAlert className="warning" size={17} /> : <Check className="success" size={17} />}<span>{status?.registryModeDetected ? "MacType 항목 감지됨" : "사용 안 함"}</span></dd></div>
        </dl>
        <div className="system-mode-note"><ShieldAlert aria-hidden="true" size={19} /><p>{status?.systemModeNote ?? "시스템 상태 확인 중"}</p></div>
      </section>

      {message && <p className="success-message">{message}</p>}
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
    </section>
  );
}
