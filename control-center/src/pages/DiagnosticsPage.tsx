import { Check, Copy, Download, ExternalLink } from "lucide-react";
import type { InstallationStatus } from "../app/model";

export function DiagnosticsPage({ status }: { status: InstallationStatus }) {
  return (
    <section className="page view-enter" aria-labelledby="diagnostics-title">
      <header className="page-header">
        <div><h1 id="diagnostics-title">진단</h1><p>구성 요소 버전과 최근 동작을 확인합니다.</p></div>
        <button className="button primary" type="button"><Download aria-hidden="true" size={17} /> 진단 파일 내보내기</button>
      </header>
      <section className="section-block" aria-labelledby="components-title">
        <div className="section-heading"><h2 id="components-title">구성 요소</h2><button className="icon-button" aria-label="정보 복사" type="button"><Copy aria-hidden="true" size={17} /></button></div>
        <dl className="detail-list diagnostic-list">
          <div><dt>Control Center</dt><dd><Check className="success" size={17} /><code>0.1.0</code></dd></div>
          <div><dt>MacType 코어</dt><dd><Check className="success" size={17} /><code>{status.coreVersion ?? "확인되지 않음"}</code></dd></div>
          <div><dt>Preview Helper</dt><dd><span className="warning-text">연결 대기 중</span></dd></div>
          <div><dt>IPC 프로토콜</dt><dd><code>MTPC v1</code></dd></div>
        </dl>
      </section>
      <section className="section-block" aria-labelledby="log-title">
        <div className="section-heading"><div><h2 id="log-title">최근 로그</h2><p>프로필 전체 내용은 기록하지 않습니다.</p></div><button className="text-action" type="button">로그 폴더 열기 <ExternalLink aria-hidden="true" size={15} /></button></div>
        <div className="log-view" role="log" aria-label="최근 진단 로그">
          <div><time>12:18:04</time><span>설치 탐색</span><p>MacType 설치 후보를 확인했습니다.</p></div>
          <div><time>12:18:04</time><span>파일 검사</span><p>32비트와 64비트 코어 파일을 확인했습니다.</p></div>
          <div data-severity="warning"><time>12:18:05</time><span>프리뷰</span><p>Preview Helper 응답을 기다리고 있습니다.</p></div>
        </div>
      </section>
    </section>
  );
}
