import { RotateCcw, Save, Search, SlidersHorizontal } from "lucide-react";
import { useState } from "react";

const groups = ["기본 설정", "글자 모양", "LCD·픽셀 배열", "글꼴별 설정", "포함·제외"];

export function ProfilesPage() {
  const [weight, setWeight] = useState(2);
  const [gamma, setGamma] = useState(1.2);

  return (
    <section className="page profile-page view-enter" aria-labelledby="profiles-title">
      <header className="page-header compact">
        <div>
          <h1 id="profiles-title">프로필</h1>
          <p>Default.ini · 저장되지 않은 변경 2개</p>
        </div>
        <div className="header-actions">
          <button className="button secondary" type="button">다른 이름으로 저장</button>
          <button className="button primary" type="button"><Save aria-hidden="true" size={17} /> 저장</button>
        </div>
      </header>

      <div className="profile-layout">
        <aside className="settings-index" aria-label="설정 구역">
          <label className="search-field">
            <Search aria-hidden="true" size={16} />
            <span className="sr-only">설정 검색</span>
            <input type="search" placeholder="설정 검색" />
          </label>
          <ul>
            {groups.map((group, index) => <li key={group}><button data-selected={index === 0} type="button">{group}</button></li>)}
          </ul>
          <label className="checkbox-row"><input type="checkbox" /> 고급 설정 표시</label>
        </aside>

        <div className="settings-workspace">
          <div className="settings-form">
            <div className="section-heading">
              <div>
                <h2>기본 설정</h2>
                <p>일반적인 글자 굵기와 감마 보정을 조정합니다.</p>
              </div>
              <button className="icon-button" aria-label="이 구역 초기화" type="button"><RotateCcw aria-hidden="true" size={17} /></button>
            </div>
            <div className="setting-row">
              <div><label htmlFor="weight">일반 글자 굵기 <span className="dirty-mark">변경됨</span></label><p>기본값 0, 허용 범위 -64에서 64</p></div>
              <div className="range-control"><input id="weight" max="64" min="-64" onChange={(event) => setWeight(Number(event.target.value))} type="range" value={weight} /><output htmlFor="weight">{weight}</output></div>
            </div>
            <div className="setting-row">
              <div><label htmlFor="gamma">감마 값 <span className="dirty-mark">변경됨</span></label><p>낮을수록 획이 진해집니다. 기본값 1.0</p></div>
              <div className="number-control"><input id="gamma" max="3" min="0.1" onChange={(event) => setGamma(Number(event.target.value))} step="0.1" type="number" value={gamma} /><span>gamma</span></div>
            </div>
            <div className="setting-row">
              <div><label htmlFor="hinting">힌팅 방식</label><p>작은 글자에서 픽셀 격자에 맞추는 방법입니다.</p></div>
              <select id="hinting" defaultValue="slight"><option value="none">사용 안 함</option><option value="slight">약하게</option><option value="full">강하게</option></select>
            </div>
          </div>

          <section className="preview-panel" aria-labelledby="preview-title">
            <div className="preview-toolbar">
              <div><SlidersHorizontal aria-hidden="true" size={17} /><h2 id="preview-title">프리뷰</h2></div>
              <div className="preview-controls"><select aria-label="프리뷰 글꼴" defaultValue="Segoe UI"><option>Segoe UI</option><option>맑은 고딕</option></select><select aria-label="프리뷰 크기" defaultValue="14"><option value="12">12 pt</option><option value="14">14 pt</option><option value="18">18 pt</option></select></div>
            </div>
            <div className="preview-canvas" role="img" aria-label="현재 설정의 글자 렌더링 프리뷰">
              <p>MacType 프리뷰 123 ABC</p>
              <p>가나다라마바사 아자차카타파하</p>
              <span>가짜 프리뷰 · Helper 연결 전</span>
            </div>
            <div className="preview-footer"><span>요청 41 · 144 DPI · RGB</span><button className="text-action" type="button">실제 창에서 보기</button></div>
          </section>
        </div>
      </div>
    </section>
  );
}
