import { AlertTriangle, CopyPlus, Plus, RotateCcw, Save, Search, SlidersHorizontal, Trash2 } from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { settingsSchema } from "../generated/settings";
import type { IndividualSetting, PreviewRequest, PreviewResult, ProfileEntry, ProfileSnapshot } from "../app/model";
import {
  duplicateProfile,
  forcePreviewCrashForCi,
  listProfiles,
  openDefaultProfile,
  openProfile,
  previewImageUrl,
  reportFrontendFailure,
  renderProfilePreview,
  saveProfile,
  setNativePreview,
  updateProfileIndividuals,
  updateProfileList,
  updateProfileSetting,
  verifyProfileWorkflowForCi,
} from "../app/tauri";

type GroupId = "basic" | "shape" | "lcd" | "individual" | "lists";

const groups: ReadonlyArray<{ id: GroupId; label: string; description: string }> = [
  { id: "basic", label: "기본 설정", description: "힌팅, 커닝과 글꼴 로딩 방식을 설정합니다." },
  { id: "shape", label: "글자 모양", description: "획 굵기, 감마와 대비를 조정합니다." },
  { id: "lcd", label: "LCD·픽셀 배열", description: "서브픽셀 순서, 필터와 채널별 튜닝을 설정합니다." },
  { id: "individual", label: "글꼴별 설정", description: "특정 글꼴에만 적용할 여섯 가지 값을 관리합니다." },
  { id: "lists", label: "포함·제외", description: "글꼴과 프로그램의 포함·제외 목록을 편집합니다." },
];

const individualLabels = ["힌팅", "AA", "일반 굵기", "굵은 굵기", "기울임", "커닝"];
const listDefinitions = [
  { kind: "excludeFonts", label: "제외 글꼴", help: "한 줄에 글꼴 이름 하나" },
  { kind: "includeFonts", label: "포함 글꼴", help: "목록이 있으면 지정한 글꼴만 처리" },
  { kind: "excludeModules", label: "제외 프로그램", help: "예: fontview.exe" },
  { kind: "includeModules", label: "포함 프로그램", help: "목록이 있으면 지정한 프로그램만 처리" },
] as const;

interface ProfilesPageProps {
  ciSmoke?: boolean;
  onPreviewReady?: () => void;
}

export function ProfilesPage({ ciSmoke = false, onPreviewReady }: ProfilesPageProps) {
  const [profile, setProfile] = useState<ProfileSnapshot | null>(null);
  const [profiles, setProfiles] = useState<ReadonlyArray<ProfileEntry>>([]);
  const [values, setValues] = useState<Record<string, number>>(
    Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])),
  );
  const [individuals, setIndividuals] = useState<IndividualSetting[]>([]);
  const [listDrafts, setListDrafts] = useState<Record<string, string>>({});
  const [activeGroup, setActiveGroup] = useState<GroupId>("basic");
  const [showAdvanced, setShowAdvanced] = useState(false);
  const [copyName, setCopyName] = useState("");
  const [newFont, setNewFont] = useState("");
  const [fontFace, setFontFace] = useState("Segoe UI");
  const [fontSize, setFontSize] = useState(14);
  const [darkPreview, setDarkPreview] = useState(false);
  const [sampleText, setSampleText] = useState("MacType 프리뷰 123 ABC\n가나다라마바사 아자차카타파하");
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
  const ciWorkflowVerified = useRef(false);

  const applySnapshot = useCallback((opened: ProfileSnapshot) => {
    setProfile(opened);
    setValues(opened.values);
    setIndividuals(opened.individuals.map((entry) => ({ ...entry, values: [...entry.values] })));
    setListDrafts({
      excludeFonts: opened.lists.excludeFonts.join("\n"),
      includeFonts: opened.lists.includeFonts.join("\n"),
      excludeModules: opened.lists.excludeModules.join("\n"),
      includeModules: opened.lists.includeModules.join("\n"),
    });
  }, []);

  useEffect(() => {
    let active = true;
    void Promise.all([openDefaultProfile(), listProfiles()])
      .then(([opened, available]) => {
        if (!active) return;
        setProfiles(available);
        if (opened) applySnapshot(opened);
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
  }, [applySnapshot]);

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
            if (ciSmoke) ciReadyRequestId.current = rendered.requestId;
            else onPreviewReady?.();
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
          text: sampleText,
          fontFace,
          fontSizePt: fontSize,
          widthPx: Math.round(width * displayScale),
          heightPx: Math.round(180 * displayScale),
          dpi: Math.round(96 * displayScale),
          foreground: darkPreview ? "#F1F3F5" : "#181D23",
          background: darkPreview ? "#171A1F" : "#EEF1F4",
        },
      };
      void drainPreviewQueue();
    }, 40);
    return () => window.clearTimeout(timer);
  }, [darkPreview, drainPreviewQueue, fontFace, fontSize, profile, sampleText, values]);

  const filteredSettings = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    return settingsSchema.filter((setting) => {
      if (!needle && setting.group !== activeGroup) return false;
      if (!needle && setting.advanced && !showAdvanced) return false;
      return !needle || `${setting.label} ${setting.description} ${setting.key}`.toLocaleLowerCase().includes(needle);
    });
  }, [activeGroup, query, showAdvanced]);

  const changeSetting = (settingId: string, value: number) => {
    setValues((current) => ({ ...current, [settingId]: value }));
    void updateProfileSetting(settingId, value)
      .then((snapshot) => {
        if (snapshot) setProfile(snapshot);
      })
      .catch((error: unknown) => setPreviewError(error instanceof Error ? error.message : String(error)));
  };

  const chooseProfile = async (path: string) => {
    try {
      applySnapshot(await openProfile(path));
      setPreviewError(null);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    }
  };

  const duplicate = async () => {
    if (!copyName.trim()) return;
    try {
      const opened = await duplicateProfile(copyName);
      applySnapshot(opened);
      setProfiles(await listProfiles());
      setCopyName("");
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    }
  };

  const save = async () => {
    setSaving(true);
    try {
      const saved = await saveProfile();
      if (saved) applySnapshot(saved);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    } finally {
      setSaving(false);
    }
  };

  const commitIndividuals = async (next: IndividualSetting[]) => {
    setIndividuals(next);
    try {
      const snapshot = await updateProfileIndividuals(next);
      if (snapshot) setProfile(snapshot);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    }
  };

  const addIndividual = () => {
    const font = newFont.trim();
    if (!font || individuals.some((entry) => entry.fontFace.toLocaleLowerCase() === font.toLocaleLowerCase())) return;
    void commitIndividuals([...individuals, { fontFace: font, values: [null, null, null, null, null, null] }]);
    setNewFont("");
  };

  const commitList = async (kind: string) => {
    const entries = (listDrafts[kind] ?? "").split(/\r?\n/).map((entry) => entry.trim()).filter(Boolean);
    try {
      const snapshot = await updateProfileList(kind, entries);
      if (snapshot) setProfile(snapshot);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
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

  const activeDefinition = groups.find((group) => group.id === activeGroup) ?? groups[0];
  const dirtyCount = profile?.dirtyKeys.length ?? 0;
  const displayScale = window.devicePixelRatio || 1;

  return (
    <section className="page profile-page view-enter" aria-labelledby="profiles-title">
      <header className="page-header compact profile-header">
        <div>
          <h1 id="profiles-title">프로필</h1>
          <p>{loading ? "프로필 검색 중" : `${profile?.path.split(/[\\/]/).pop() ?? "프로필 없음"} · 저장되지 않은 변경 ${dirtyCount}개`}</p>
        </div>
        <div className="header-actions profile-actions">
          <select aria-label="프로필 선택" disabled={profiles.length === 0} onChange={(event) => void chooseProfile(event.target.value)} value={profile?.path ?? ""}>
            {profiles.map((entry) => <option key={entry.path} value={entry.path}>{entry.name}</option>)}
          </select>
          <input aria-label="복제 프로필 이름" onChange={(event) => setCopyName(event.target.value)} placeholder="새 프로필 이름" value={copyName} />
          <button className="button secondary" disabled={!profile || !copyName.trim()} onClick={() => void duplicate()} type="button"><CopyPlus aria-hidden="true" size={16} /> 복제</button>
          <button className="button primary" disabled={!profile || dirtyCount === 0 || saving} onClick={() => void save()} type="button"><Save aria-hidden="true" size={17} /> {saving ? "저장 중" : "저장"}</button>
        </div>
      </header>

      <div className="profile-layout">
        <aside className="settings-index" aria-label="설정 구역">
          <label className="search-field"><Search aria-hidden="true" size={16} /><span className="sr-only">설정 검색</span><input onChange={(event) => setQuery(event.target.value)} placeholder="설정 검색" type="search" value={query} /></label>
          <ul>{groups.map((group) => <li key={group.id}><button data-selected={!query && activeGroup === group.id} onClick={() => { setActiveGroup(group.id); setQuery(""); }} type="button">{group.label}</button></li>)}</ul>
          <label className="checkbox-row"><input checked={showAdvanced} onChange={(event) => setShowAdvanced(event.target.checked)} type="checkbox" /> 고급 설정 표시</label>
        </aside>

        <div className="settings-workspace">
          <div className="settings-form">
            <div className="section-heading"><div><h2>{query ? "검색 결과" : activeDefinition.label}</h2><p>{query ? `“${query}”와 일치하는 모든 구역의 설정입니다.` : activeDefinition.description}</p></div></div>

            {(query || activeGroup === "basic" || activeGroup === "shape" || activeGroup === "lcd") && filteredSettings.map((setting) => {
              const value = values[setting.id] ?? setting.default;
              const dirty = profile?.dirtyKeys.includes(setting.id) ?? false;
              return (
                <div className="setting-row" key={setting.id}>
                  <div><label htmlFor={setting.id}>{setting.label} {dirty && <span className="dirty-mark">변경됨</span>}</label><p>{setting.description} 기본값 {setting.default}, 허용 범위 {setting.min}–{setting.max}{setting.apply === "restart_required" ? " · 재시작 필요" : ""}</p></div>
                  <div className="range-control">
                    {setting.control === "select" && "options" in setting ? (
                      <select id={setting.id} onChange={(event) => changeSetting(setting.id, Number(event.target.value))} value={value}>{setting.options.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}</select>
                    ) : setting.control === "boolean" ? (
                      <label className="switch-control"><input checked={value === 1} id={setting.id} onChange={(event) => changeSetting(setting.id, event.target.checked ? 1 : 0)} type="checkbox" /><span>{value === 1 ? "사용" : "사용 안 함"}</span></label>
                    ) : (
                      <input id={setting.id} max={setting.max} min={setting.min} onChange={(event) => changeSetting(setting.id, Number(event.target.value))} step={setting.type === "integer" ? 1 : 0.01} type="range" value={value} />
                    )}
                    <output htmlFor={setting.id}>{value}{setting.unit === "px" ? " px" : ""}</output>
                    <button className="icon-button" aria-label={`${setting.label} 초기화`} onClick={() => changeSetting(setting.id, setting.default)} type="button"><RotateCcw aria-hidden="true" size={15} /></button>
                  </div>
                </div>
              );
            })}

            {!query && activeGroup === "individual" && (
              <div className="collection-editor">
                <div className="inline-create"><input aria-label="추가할 글꼴 이름" onChange={(event) => setNewFont(event.target.value)} placeholder="예: Segoe UI" value={newFont} /><button className="button secondary" onClick={addIndividual} type="button"><Plus aria-hidden="true" size={16} /> 글꼴 추가</button></div>
                {individuals.map((entry, rowIndex) => (
                  <div className="individual-row" key={`${entry.fontFace}-${rowIndex}`}>
                    <strong>{entry.fontFace}</strong>
                    <div>{individualLabels.map((label, valueIndex) => <label key={label}><span>{label}</span><input aria-label={`${entry.fontFace} ${label}`} max={valueIndex === 2 ? 64 : valueIndex === 3 || valueIndex === 4 ? 32 : valueIndex === 1 ? 6 : valueIndex === 0 ? 2 : 1} min={valueIndex === 2 ? -64 : valueIndex === 3 || valueIndex === 4 ? -32 : valueIndex === 1 ? -1 : 0} onChange={(event) => { const next = individuals.map((item) => ({ ...item, values: [...item.values] })); next[rowIndex].values[valueIndex] = event.target.value === "" ? null : Number(event.target.value); void commitIndividuals(next); }} placeholder="상속" type="number" value={entry.values[valueIndex] ?? ""} /></label>)}</div>
                    <button className="icon-button" aria-label={`${entry.fontFace} 제거`} onClick={() => void commitIndividuals(individuals.filter((_, index) => index !== rowIndex))} type="button"><Trash2 aria-hidden="true" size={15} /></button>
                  </div>
                ))}
                {individuals.length === 0 && <p className="empty-state">글꼴별 설정이 없습니다. 비워 둔 값은 기본 설정을 상속합니다.</p>}
              </div>
            )}

            {!query && activeGroup === "lists" && <div className="list-grid">{listDefinitions.map((definition) => <label key={definition.kind}><strong>{definition.label}</strong><span>{definition.help}</span><textarea onBlur={() => void commitList(definition.kind)} onChange={(event) => setListDrafts((current) => ({ ...current, [definition.kind]: event.target.value }))} rows={6} value={listDrafts[definition.kind] ?? ""} /></label>)}</div>}
          </div>

          <section className="preview-panel" aria-labelledby="preview-title">
            <div className="preview-toolbar"><div><SlidersHorizontal aria-hidden="true" size={17} /><h2 id="preview-title">프리뷰</h2></div><div className="preview-controls"><select aria-label="프리뷰 글꼴" onChange={(event) => setFontFace(event.target.value)} value={fontFace}><option>Segoe UI</option><option>맑은 고딕</option><option>Tahoma</option></select><select aria-label="프리뷰 크기" onChange={(event) => setFontSize(Number(event.target.value))} value={fontSize}><option value="12">12 pt</option><option value="14">14 pt</option><option value="18">18 pt</option></select><button className="text-action" onClick={() => setDarkPreview((current) => !current)} type="button">{darkPreview ? "밝은 배경" : "어두운 배경"}</button></div></div>
            <textarea className="sample-input" aria-label="프리뷰 예시 문장" onChange={(event) => setSampleText(event.target.value)} rows={2} value={sampleText} />
            <div className="preview-canvas" data-dark={darkPreview} ref={canvasRef} role="img" aria-label="현재 설정의 글자 렌더링 프리뷰">
              {preview ? <img alt="MacType Helper가 렌더링한 글자 프리뷰" height={preview.height / displayScale} onLoad={() => { if (ciSmoke && ciReadyRequestId.current === preview.requestId && !ciWorkflowVerified.current) { ciWorkflowVerified.current = true; void verifyProfileWorkflowForCi().then(() => onPreviewReady?.()).catch((error: unknown) => { const message = error instanceof Error ? error.message : String(error); setPreviewError(message); void reportFrontendFailure("profiles", message); }); } }} src={previewImageUrl(preview.imagePath)} width={preview.width / displayScale} /> : <><p>MacType 프리뷰 123 ABC</p><p>가나다라마바사 아자차카타파하</p><span>Helper 응답 대기 중</span></>}
            </div>
            {previewError && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {previewError}</p>}
            <div className="preview-footer"><span>{preview ? `요청 ${preview.requestId} · ${preview.dpi} DPI · ${preview.elapsedMs} ms` : "프리뷰 준비 중"}</span><button className="text-action" onClick={() => void toggleNativePreview()} type="button">{nativeVisible ? "실제 창 닫기" : "실제 창에서 보기"}</button></div>
          </section>
        </div>
      </div>
    </section>
  );
}
