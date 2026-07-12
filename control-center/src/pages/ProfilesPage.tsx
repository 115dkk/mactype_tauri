import { AlertTriangle, RotateCcw, Save, Search, SlidersHorizontal } from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { settingsSchema } from "../generated/settings";
import type { PreviewRequest, PreviewResult, ProfileSnapshot } from "../app/model";
import {
  openDefaultProfile,
  forcePreviewCrashForCi,
  previewImageUrl,
  renderProfilePreview,
  saveProfile,
  setNativePreview,
  updateProfileSetting,
} from "../app/tauri";

const groups = ["기본 설정", "글자 모양", "LCD·픽셀 배열", "글꼴별 설정", "포함·제외"];

interface ProfilesPageProps {
  ciSmoke?: boolean;
  onPreviewReady?: () => void;
}

export function ProfilesPage({ ciSmoke = false, onPreviewReady }: ProfilesPageProps) {
  const [profile, setProfile] = useState<ProfileSnapshot | null>(null);
  const [values, setValues] = useState<Record<string, number>>(
    Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])),
  );
  const [fontFace, setFontFace] = useState("Segoe UI");
  const [fontSize, setFontSize] = useState(14);
  const [preview, setPreview] = useState<PreviewResult | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [nativeVisible, setNativeVisible] = useState(false);
  const [query, setQuery] = useState("");
  const canvasRef = useRef<HTMLDivElement>(null);
  const pendingPreview = useRef<PreviewRequest | null>(null);
  const previewRunning = useRef(false);
  const newestResponse = useRef(0);
  const restartVerified = useRef(false);
  const ciReadyRequestId = useRef<number | null>(null);

  useEffect(() => {
    let active = true;
    void openDefaultProfile()
      .then((opened) => {
        if (!active || !opened) return;
        setProfile(opened);
        setValues(opened.values);
      })
      .catch((error: unknown) => {
        if (active) setPreviewError(error instanceof Error ? error.message : String(error));
      })
      .finally(() => {
        if (active) setLoading(false);
      });
    return () => {
      active = false;
    };
  }, []);

  const drainPreviewQueue = useCallback(async () => {
    if (previewRunning.current) return;
    previewRunning.current = true;
    try {
      while (pendingPreview.current) {
        const request = pendingPreview.current;
        pendingPreview.current = null;
        try {
          const rendered = await renderProfilePreview(request);
          if (rendered && rendered.requestId > newestResponse.current) {
            newestResponse.current = rendered.requestId;
            setPreview(rendered);
            setPreviewError(null);
            if (ciSmoke && !restartVerified.current) {
              restartVerified.current = true;
              await forcePreviewCrashForCi();
              pendingPreview.current = request;
              continue;
            }
            if (ciSmoke) {
              ciReadyRequestId.current = rendered.requestId;
            } else {
              onPreviewReady?.();
            }
          }
        } catch (error: unknown) {
          setPreviewError(error instanceof Error ? error.message : String(error));
        }
      }
    } finally {
      previewRunning.current = false;
    }
  }, [ciSmoke, onPreviewReady]);

  useEffect(() => {
    if (!profile) return undefined;
    const timer = window.setTimeout(() => {
      const displayScale = window.devicePixelRatio || 1;
      const width = Math.max(320, canvasRef.current?.clientWidth ?? 760);
      pendingPreview.current = {
        profilePath: profile.path,
        overrides: values,
        displayScale,
        sample: {
          text: "MacType 프리뷰 123 ABC\n가나다라마바사 아자차카타파하",
          fontFace,
          fontSizePt: fontSize,
          widthPx: Math.round(width * displayScale),
          heightPx: Math.round(180 * displayScale),
          dpi: Math.round(96 * displayScale),
          foreground: "#181D23",
          background: "#EEF1F4",
        },
      };
      void drainPreviewQueue();
    }, 40);
    return () => window.clearTimeout(timer);
  }, [drainPreviewQueue, fontFace, fontSize, profile, values]);

  const filteredSettings = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    if (!needle) return settingsSchema;
    return settingsSchema.filter((setting) =>
      `${setting.label} ${setting.description} ${setting.key}`.toLocaleLowerCase().includes(needle),
    );
  }, [query]);

  const changeSetting = (settingId: string, value: number) => {
    setValues((current) => ({ ...current, [settingId]: value }));
    void updateProfileSetting(settingId, value)
      .then((snapshot) => {
        if (snapshot) setProfile(snapshot);
      })
      .catch((error: unknown) => setPreviewError(error instanceof Error ? error.message : String(error)));
  };

  const resetSetting = (settingId: string, defaultValue: number) => changeSetting(settingId, defaultValue);

  const save = async () => {
    setSaving(true);
    try {
      const saved = await saveProfile();
      if (saved) setProfile(saved);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    } finally {
      setSaving(false);
    }
  };

  const toggleNativePreview = async () => {
    try {
      const visible = await setNativePreview(!nativeVisible);
      setNativeVisible(visible);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    }
  };

  const dirtyCount = profile?.dirtyKeys.length ?? 0;
  const displayScale = window.devicePixelRatio || 1;

  return (
    <section className="page profile-page view-enter" aria-labelledby="profiles-title">
      <header className="page-header compact">
        <div>
          <h1 id="profiles-title">프로필</h1>
          <p>{loading ? "프로필 검색 중" : `${profile?.path.split(/[\\/]/).pop() ?? "프로필 없음"} · 저장되지 않은 변경 ${dirtyCount}개`}</p>
        </div>
        <div className="header-actions">
          <button className="button secondary" disabled={!profile} type="button">다른 이름으로 저장</button>
          <button className="button primary" disabled={!profile || dirtyCount === 0 || saving} onClick={() => void save()} type="button">
            <Save aria-hidden="true" size={17} /> {saving ? "저장 중" : "저장"}
          </button>
        </div>
      </header>

      <div className="profile-layout">
        <aside className="settings-index" aria-label="설정 구역">
          <label className="search-field">
            <Search aria-hidden="true" size={16} />
            <span className="sr-only">설정 검색</span>
            <input onChange={(event) => setQuery(event.target.value)} placeholder="설정 검색" type="search" value={query} />
          </label>
          <ul>{groups.map((group, index) => <li key={group}><button data-selected={index === 0} type="button">{group}</button></li>)}</ul>
          <label className="checkbox-row"><input type="checkbox" /> 고급 설정 표시</label>
        </aside>

        <div className="settings-workspace">
          <div className="settings-form">
            <div className="section-heading">
              <div><h2>기본 설정</h2><p>설정 명세의 범위와 기본값을 사용해 실제 프로필을 편집합니다.</p></div>
            </div>
            {filteredSettings.map((setting) => {
              const value = values[setting.id] ?? setting.default;
              const dirty = profile?.dirtyKeys.includes(setting.id) ?? false;
              return (
                <div className="setting-row" key={setting.id}>
                  <div>
                    <label htmlFor={setting.id}>{setting.label} {dirty && <span className="dirty-mark">변경됨</span>}</label>
                    <p>{setting.description} 기본값 {setting.default}, 허용 범위 {setting.min}에서 {setting.max}</p>
                  </div>
                  <div className="range-control">
                    <input
                      id={setting.id}
                      max={setting.max}
                      min={setting.min}
                      onChange={(event) => changeSetting(setting.id, Number(event.target.value))}
                      step={setting.type === "integer" ? 1 : 0.01}
                      type="range"
                      value={value}
                    />
                    <output htmlFor={setting.id}>{value}</output>
                    <button className="icon-button" aria-label={`${setting.label} 초기화`} onClick={() => resetSetting(setting.id, setting.default)} type="button">
                      <RotateCcw aria-hidden="true" size={15} />
                    </button>
                  </div>
                </div>
              );
            })}
          </div>

          <section className="preview-panel" aria-labelledby="preview-title">
            <div className="preview-toolbar">
              <div><SlidersHorizontal aria-hidden="true" size={17} /><h2 id="preview-title">프리뷰</h2></div>
              <div className="preview-controls">
                <select aria-label="프리뷰 글꼴" onChange={(event) => setFontFace(event.target.value)} value={fontFace}><option>Segoe UI</option><option>맑은 고딕</option></select>
                <select aria-label="프리뷰 크기" onChange={(event) => setFontSize(Number(event.target.value))} value={fontSize}><option value="12">12 pt</option><option value="14">14 pt</option><option value="18">18 pt</option></select>
              </div>
            </div>
            <div className="preview-canvas" ref={canvasRef} role="img" aria-label="현재 설정의 글자 렌더링 프리뷰">
              {preview ? (
                <img
                  alt="MacType Helper가 렌더링한 글자 프리뷰"
                  height={preview.height / displayScale}
                  onLoad={() => {
                    if (ciSmoke && ciReadyRequestId.current === preview.requestId) {
                      onPreviewReady?.();
                    }
                  }}
                  src={previewImageUrl(preview.imagePath)}
                  width={preview.width / displayScale}
                />
              ) : (
                <><p>MacType 프리뷰 123 ABC</p><p>가나다라마바사 아자차카타파하</p><span>Helper 응답 대기 중</span></>
              )}
            </div>
            {previewError && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {previewError}</p>}
            <div className="preview-footer">
              <span>{preview ? `요청 ${preview.requestId} · ${preview.dpi} DPI · ${preview.elapsedMs} ms` : "프리뷰 준비 중"}</span>
              <button className="text-action" onClick={() => void toggleNativePreview()} type="button">{nativeVisible ? "실제 창 닫기" : "실제 창에서 보기"}</button>
            </div>
          </section>
        </div>
      </div>
    </section>
  );
}
