import { AlertTriangle, ArrowRight, Check, FolderSearch } from "lucide-react";
import type { InstallationStatus } from "../app/model";

export function OverviewPage({ status, onOpenProfiles }: { status: InstallationStatus; onOpenProfiles: () => void }) {
  return (
    <section className="page view-enter" aria-labelledby="overview-title">
      <header className="page-header">
        <div>
          <h1 id="overview-title">개요</h1>
          <p>현재 설치와 프로필 상태를 확인합니다.</p>
        </div>
        <button className="button secondary" type="button">
          <FolderSearch aria-hidden="true" size={17} />
          설치 위치 다시 찾기
        </button>
      </header>

      <div className="status-band" data-state={status.state}>
        <AlertTriangle aria-hidden="true" size={20} />
        <div>
          <strong>프리뷰 연결이 필요합니다</strong>
          <span>코어 파일은 확인했지만 32비트 프리뷰 프로세스가 아직 시작되지 않았습니다.</span>
        </div>
        <button className="button primary" type="button">다시 연결</button>
      </div>

      <section className="section-block" aria-labelledby="installation-title">
        <div className="section-heading">
          <h2 id="installation-title">설치 구성</h2>
          <code>{status.root ?? "설치 위치 없음"}</code>
        </div>
        <dl className="detail-list">
          {status.findings.map((finding) => (
            <div key={finding.label}>
              <dt>{finding.label}</dt>
              <dd>
                {finding.ok ? <Check className="success" aria-label="확인됨" size={17} /> : <AlertTriangle className="warning" aria-label="확인 필요" size={17} />}
                <span>{finding.value}</span>
              </dd>
            </div>
          ))}
        </dl>
      </section>

      <section className="split-section" aria-labelledby="next-title">
        <div>
          <h2 id="next-title">다음 작업</h2>
          <p>기존 INI 프로필을 열어 설정을 검토합니다. 저장하기 전까지 원본 파일은 변경되지 않습니다.</p>
        </div>
        <button className="text-action" onClick={onOpenProfiles} type="button">
          프로필 열기 <ArrowRight aria-hidden="true" size={17} />
        </button>
      </section>
    </section>
  );
}
