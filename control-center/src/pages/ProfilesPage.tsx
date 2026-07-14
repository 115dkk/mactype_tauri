import { AlertTriangle, Search, SlidersHorizontal } from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { settingsSchema } from "../generated/settings";
import type { AdvancedProfile, IndividualSetting, PreviewRequest, PreviewResult, ProfileSnapshot } from "../app/model";
import { settingMessageKey, useI18n } from "../i18n/i18n";
import {
  currentProfile,
  forcePreviewCrashForCi,
  loadInstalledFontFamilies,
  openDefaultProfile,
  previewImageUrl,
  reportFrontendFailure,
  renderProfilePreview,
  setNativePreview,
  updateProfileIndividuals,
  updateProfileAdvanced,
  updateProfileList,
  updateProfileSetting,
  verifyProfileWorkflowForCi,
} from "../app/tauri";
import { AdvancedSettings } from "./profiles/AdvancedSettings";
import { IndividualSettings } from "./profiles/IndividualSettings";
import { ListsEditor } from "./profiles/ListsEditor";
import { BasicSettings, LcdSettings, SearchSettings, ShapeSettings } from "./profiles/SchemaSettings";
import { splitSubstitution } from "./profiles/profileEditorUtils";

type GroupId = "basic" | "shape" | "lcd" | "advanced" | "individual" | "lists";

interface ProfilesPageProps {
  ciSmoke?: boolean;
  onPreviewReady?: () => void;
}

export function ProfilesPage({ ciSmoke = false, onPreviewReady }: ProfilesPageProps) {
  const { locale, t } = useI18n();
  const groups = useMemo<ReadonlyArray<{ id: GroupId; label: string; description: string }>>(() => [
    { id: "basic", label: t("group.basic.label"), description: t("group.basic.description") },
    { id: "shape", label: t("group.shape.label"), description: t("group.shape.description") },
    { id: "lcd", label: t("group.lcd.label"), description: t("group.lcd.description") },
    { id: "advanced", label: t("group.advanced.label"), description: t("group.advanced.description") },
    { id: "individual", label: t("group.individual.label"), description: t("group.individual.description") },
    { id: "lists", label: t("group.lists.label"), description: t("group.lists.description") },
  ], [t]);
  const individualLabels = useMemo(() => [
    t("individual.hinting"), t("individual.aa"), t("individual.normalWeight"),
    t("individual.boldWeight"), t("individual.slant"), t("individual.kerning"),
  ], [t]);
  const listDefinitions = useMemo(() => [
    { kind: "excludeFonts", label: t("list.excludeFonts.label"), help: t("list.excludeFonts.help") },
    { kind: "includeFonts", label: t("list.includeFonts.label"), help: t("list.includeFonts.help") },
    { kind: "excludeModules", label: t("list.excludeModules.label"), help: t("list.excludeModules.help") },
    { kind: "includeModules", label: t("list.includeModules.label"), help: t("list.includeModules.help") },
    { kind: "unloadDlls", label: t("list.unloadDlls.label"), help: t("list.unloadDlls.help") },
    { kind: "excludeSubstitutionModules", label: t("list.excludeSubstitutionModules.label"), help: t("list.excludeSubstitutionModules.help") },
  ] as const, [t]);
  const [profile, setProfile] = useState<ProfileSnapshot | null>(null);
  const [values, setValues] = useState<Record<string, number>>(
    Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])),
  );
  const [individuals, setIndividuals] = useState<IndividualSetting[]>([]);
  const [listDrafts, setListDrafts] = useState<Record<string, string>>({});
  const [advanced, setAdvanced] = useState<AdvancedProfile>({ shadow: null, lcdFilterWeight: null, pixelLayout: null, displayAffinity: [], fontSubstitutes: [], infinalityGammaCorrection: [0, 100], infinalityFilterParams: [11, 22, 38, 22, 11] });
  const [activeGroup, setActiveGroup] = useState<GroupId>("basic");
  const [showAdvanced, setShowAdvanced] = useState(false);
  const [installedFonts, setInstalledFonts] = useState<ReadonlyArray<string>>([]);
  const [fontFace, setFontFace] = useState("Segoe UI");
  const [fontSize, setFontSize] = useState(14);
  const [darkPreview, setDarkPreview] = useState(false);
  const [sampleText, setSampleText] = useState(() => t("profiles.sampleText"));
  const previousDefaultSample = useRef(sampleText);
  const [preview, setPreview] = useState<PreviewResult | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [nativeVisible, setNativeVisible] = useState(false);
  const [query, setQuery] = useState("");
  const canvasRef = useRef<HTMLDivElement>(null);
  const pendingPreview = useRef<PreviewRequest | null>(null);
  const previewRunning = useRef(false);
  const newestResponse = useRef(0);
  const restartVerified = useRef(false);
  const ciReadyRequestId = useRef<number | null>(null);
  const ciWorkflowVerified = useRef(false);

  const fontFamilies = useMemo(() => {
    const referenced = [
      fontFace,
      ...individuals.map((entry) => entry.fontFace),
      ...advanced.fontSubstitutes.flatMap((mapping) => {
        const pair = splitSubstitution(mapping);
        return [pair.source, pair.replacement];
      }),
      ...(listDrafts.excludeFonts ?? "").split(/\r?\n/),
      ...(listDrafts.includeFonts ?? "").split(/\r?\n/),
    ].map((font) => font.trim()).filter(Boolean);
    const collator = new Intl.Collator(locale, { sensitivity: "base", numeric: true });
    return [...new Set([...installedFonts, ...referenced])]
      .sort((left, right) => collator.compare(left, right));
  }, [advanced.fontSubstitutes, fontFace, individuals, installedFonts, listDrafts.excludeFonts, listDrafts.includeFonts, locale]);
  const installedFontKeys = useMemo(() => new Set(installedFonts.map((font) => font.toLocaleLowerCase())), [installedFonts]);
  const fontOptionLabel = (font: string) => installedFontKeys.has(font.toLocaleLowerCase())
    ? font
    : `${font} · ${t("profiles.fontUnavailable")}`;

  useEffect(() => {
    const nextDefault = t("profiles.sampleText");
    setSampleText((current) => current === previousDefaultSample.current ? nextDefault : current);
    previousDefaultSample.current = nextDefault;
  }, [locale, t]);

  const applySnapshot = useCallback((opened: ProfileSnapshot) => {
    setProfile(opened);
    setValues(opened.values);
    setIndividuals(opened.individuals.map((entry) => ({ ...entry, values: [...entry.values] })));
    setListDrafts({
      excludeFonts: opened.lists.excludeFonts.join("\n"),
      includeFonts: opened.lists.includeFonts.join("\n"),
      excludeModules: opened.lists.excludeModules.join("\n"),
      includeModules: opened.lists.includeModules.join("\n"),
      unloadDlls: opened.lists.unloadDlls.join("\n"),
      excludeSubstitutionModules: opened.lists.excludeSubstitutionModules.join("\n"),
    });
    setAdvanced({
      ...opened.advanced,
      shadow: opened.advanced.shadow ? { ...opened.advanced.shadow } : null,
      lcdFilterWeight: opened.advanced.lcdFilterWeight ? [...opened.advanced.lcdFilterWeight] : null,
      pixelLayout: opened.advanced.pixelLayout ? [...opened.advanced.pixelLayout] : null,
      displayAffinity: [...opened.advanced.displayAffinity],
      fontSubstitutes: [...opened.advanced.fontSubstitutes],
      infinalityGammaCorrection: [...opened.advanced.infinalityGammaCorrection],
      infinalityFilterParams: [...opened.advanced.infinalityFilterParams],
    });
  }, []);

  useEffect(() => {
    let active = true;
    void currentProfile()
      .then(async (current) => current ?? await openDefaultProfile())
      .then((opened) => {
        if (!active) return;
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

  useEffect(() => {
    let active = true;
    void loadInstalledFontFamilies()
      .then((families) => {
        if (!active) return;
        setInstalledFonts(families);
        setFontFace((current) => families.some((font) => font.toLocaleLowerCase() === current.toLocaleLowerCase()) ? current : families[0] ?? current);
      })
      .catch((error: unknown) => {
        if (active) setPreviewError(error instanceof Error ? error.message : String(error));
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
      const localized = `${t(settingMessageKey(setting.id, "label"))} ${t(settingMessageKey(setting.id, "description"))} ${setting.key}`;
      return !needle || localized.toLocaleLowerCase().includes(needle);
    });
  }, [activeGroup, query, showAdvanced, t]);

  const changeSetting = (settingId: string, value: number) => {
    setValues((current) => ({ ...current, [settingId]: value }));
    void updateProfileSetting(settingId, value)
      .then((snapshot) => {
        if (snapshot) setProfile(snapshot);
      })
      .catch((error: unknown) => setPreviewError(error instanceof Error ? error.message : String(error)));
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

  const addIndividual = (font: string) => {
    font = font.trim();
    if (!font || individuals.some((entry) => entry.fontFace.toLocaleLowerCase() === font.toLocaleLowerCase())) return;
    void commitIndividuals([...individuals, { fontFace: font, values: [null, null, null, null, null, null] }]);
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

  const commitAdvanced = async (next: AdvancedProfile) => {
    setAdvanced(next);
    try {
      const snapshot = await updateProfileAdvanced(next);
      if (snapshot) setProfile(snapshot);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    }
  };

  const updateFontList = async (kind: "excludeFonts" | "includeFonts", entries: ReadonlyArray<string>) => {
    setListDrafts((current) => ({ ...current, [kind]: entries.join("\n") }));
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
          <h1 id="profiles-title">{t("nav.profiles")}</h1>
          <p>{loading ? t("profiles.searching") : t("profiles.summary", { name: profile?.path.split(/[\\/]/).pop() ?? t("profiles.none"), count: dirtyCount })}</p>
        </div>
      </header>

      <div className="profile-layout">
        <aside className="settings-index" aria-label={t("profiles.sections")}>
          <label className="search-field"><Search aria-hidden="true" size={16} /><span className="sr-only">{t("profiles.search")}</span><input onChange={(event) => setQuery(event.target.value)} placeholder={t("profiles.search")} type="search" value={query} /></label>
          <ul>{groups.map((group) => <li key={group.id}><button data-selected={!query && activeGroup === group.id} onClick={() => { setActiveGroup(group.id); setQuery(""); }} type="button">{group.label}</button></li>)}</ul>
          <label className="checkbox-row"><input checked={showAdvanced} onChange={(event) => setShowAdvanced(event.target.checked)} type="checkbox" /> {t("profiles.showAdvanced")}</label>
        </aside>

        <div className="settings-workspace">
          <div className="settings-form">
            <div className="section-heading"><div><h2>{query ? t("profiles.searchResults") : activeDefinition.label}</h2><p>{query ? t("profiles.searchDescription", { query }) : activeDefinition.description}</p></div></div>

            {query && <SearchSettings dirtyKeys={profile?.dirtyKeys ?? []} onChange={changeSetting} settings={filteredSettings} t={t} values={values} />}
            {!query && activeGroup === "basic" && <BasicSettings dirtyKeys={profile?.dirtyKeys ?? []} onChange={changeSetting} settings={filteredSettings} t={t} values={values} />}
            {!query && activeGroup === "shape" && <ShapeSettings dirtyKeys={profile?.dirtyKeys ?? []} onChange={changeSetting} settings={filteredSettings} t={t} values={values} />}
            {!query && activeGroup === "lcd" && <LcdSettings dirtyKeys={profile?.dirtyKeys ?? []} onChange={changeSetting} settings={filteredSettings} t={t} values={values} />}

            {!query && activeGroup === "advanced" && (
              <AdvancedSettings
                advanced={advanced}
                dirtyKeys={profile?.dirtyKeys ?? []}
                fontFamilies={fontFamilies}
                fontOptionLabel={fontOptionLabel}
                onAdvancedChange={setAdvanced}
                onAdvancedCommit={(next) => void commitAdvanced(next)}
                onSettingChange={changeSetting}
                settings={filteredSettings}
                t={t}
                values={values}
              />
            )}

            {!query && activeGroup === "individual" && (
              <IndividualSettings
                fontFamilies={fontFamilies}
                individualLabels={individualLabels}
                individuals={individuals}
                installedFontKeys={installedFontKeys}
                onAdd={addIndividual}
                onCommit={(next) => void commitIndividuals(next)}
                t={t}
              />
            )}

            {!query && activeGroup === "lists" && (
              <ListsEditor
                definitions={listDefinitions}
                drafts={listDrafts}
                fontFamilies={fontFamilies}
                fontOptionLabel={fontOptionLabel}
                installedFontKeys={installedFontKeys}
                onCommit={(kind) => void commitList(kind)}
                onDraftChange={(kind, value) => setListDrafts((current) => ({ ...current, [kind]: value }))}
                onUpdateFontList={(kind, entries) => void updateFontList(kind, entries)}
                t={t}
              />
            )}
          </div>

          <section className="preview-panel" aria-labelledby="preview-title">
            <div className="preview-toolbar"><div><SlidersHorizontal aria-hidden="true" size={17} /><h2 id="preview-title">{t("profiles.preview")}</h2></div><div className="preview-controls"><select aria-label={t("profiles.previewFont")} onChange={(event) => setFontFace(event.target.value)} value={fontFace}>{fontFamilies.map((font) => <option key={font} value={font}>{fontOptionLabel(font)}</option>)}</select><select aria-label={t("profiles.previewSize")} onChange={(event) => setFontSize(Number(event.target.value))} value={fontSize}><option value="12">12 pt</option><option value="14">14 pt</option><option value="18">18 pt</option></select><button className="text-action" onClick={() => setDarkPreview((current) => !current)} type="button">{darkPreview ? t("profiles.lightBackground") : t("profiles.darkBackground")}</button></div></div>
            <textarea className="sample-input" aria-label={t("profiles.sampleAria")} onChange={(event) => setSampleText(event.target.value)} rows={2} value={sampleText} />
            <div className="preview-canvas" data-dark={darkPreview} ref={canvasRef} role="img" aria-label={t("profiles.previewAria")}>
              {preview ? <img alt={t("profiles.previewImageAlt")} height={preview.height / displayScale} onLoad={() => { if (ciSmoke && ciReadyRequestId.current === preview.requestId && !ciWorkflowVerified.current) { ciWorkflowVerified.current = true; void verifyProfileWorkflowForCi().then(() => onPreviewReady?.()).catch((error: unknown) => { const message = error instanceof Error ? error.message : String(error); setPreviewError(message); void reportFrontendFailure("profiles", message); }); } }} src={previewImageUrl(preview.imagePath)} width={preview.width / displayScale} /> : <><p>{t("profiles.sampleText").split("\n")[0]}</p><p>{t("profiles.sampleText").split("\n")[1]}</p><span>{t("profiles.helperWaiting")}</span></>}
            </div>
            {previewError && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {previewError}</p>}
            <div className="preview-footer"><span>{preview ? t("profiles.previewRequest", { request: preview.requestId, dpi: preview.dpi, elapsed: preview.elapsedMs }) : t("profiles.previewReady")}</span><button className="text-action" onClick={() => void toggleNativePreview()} type="button">{nativeVisible ? t("profiles.closeNative") : t("profiles.openNative")}</button></div>
          </section>
        </div>
      </div>
    </section>
  );
}
