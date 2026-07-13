import { AlertTriangle, ArrowRight, CopyPlus, Play, Plus, RotateCcw, Save, Search, SlidersHorizontal, Trash2 } from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { settingsSchema } from "../generated/settings";
import type { AdvancedProfile, IndividualSetting, PreviewRequest, PreviewResult, ProfileEntry, ProfileSnapshot } from "../app/model";
import { settingMessageKey, settingOptionMessageKey, useI18n } from "../i18n/i18n";
import {
  applyOpenProfile,
  duplicateProfile,
  forcePreviewCrashForCi,
  loadInstalledFontFamilies,
  listProfiles,
  openDefaultProfile,
  openProfile,
  previewImageUrl,
  reportFrontendFailure,
  renderProfilePreview,
  saveProfile,
  setNativePreview,
  updateProfileIndividuals,
  updateProfileAdvanced,
  updateProfileList,
  updateProfileSetting,
  verifyProfileWorkflowForCi,
} from "../app/tauri";

type GroupId = "basic" | "shape" | "lcd" | "advanced" | "individual" | "lists";

const splitSubstitution = (mapping: string) => {
  const separator = mapping.indexOf("=");
  return separator < 0
    ? { source: mapping, replacement: mapping }
    : { source: mapping.slice(0, separator).trim(), replacement: mapping.slice(separator + 1).trim() };
};

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
  const [profiles, setProfiles] = useState<ReadonlyArray<ProfileEntry>>([]);
  const [values, setValues] = useState<Record<string, number>>(
    Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])),
  );
  const [individuals, setIndividuals] = useState<IndividualSetting[]>([]);
  const [listDrafts, setListDrafts] = useState<Record<string, string>>({});
  const [advanced, setAdvanced] = useState<AdvancedProfile>({ shadow: null, lcdFilterWeight: null, pixelLayout: null, displayAffinity: [], fontSubstitutes: [], infinalityGammaCorrection: [0, 100], infinalityFilterParams: [11, 22, 38, 22, 11] });
  const [activeGroup, setActiveGroup] = useState<GroupId>("basic");
  const [showAdvanced, setShowAdvanced] = useState(false);
  const [copyName, setCopyName] = useState("");
  const [installedFonts, setInstalledFonts] = useState<ReadonlyArray<string>>([]);
  const [fontFace, setFontFace] = useState("Segoe UI");
  const [fontSize, setFontSize] = useState(14);
  const [darkPreview, setDarkPreview] = useState(false);
  const [sampleText, setSampleText] = useState(() => t("profiles.sampleText"));
  const previousDefaultSample = useRef(sampleText);
  const [preview, setPreview] = useState<PreviewResult | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [applying, setApplying] = useState(false);
  const [appliedProfile, setAppliedProfile] = useState<string | null>(null);
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

  const apply = async () => {
    setApplying(true);
    try {
      const applied = await applyOpenProfile();
      setAppliedProfile(applied.sourceProfile);
      setPreviewError(null);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    } finally {
      setApplying(false);
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

  const updateSubstitution = (index: number, key: "source" | "replacement", value: string) => {
    const substitutions = advanced.fontSubstitutes.map((mapping, currentIndex) => {
      if (currentIndex !== index) return mapping;
      const pair = splitSubstitution(mapping);
      return `${key === "source" ? value : pair.source}=${key === "replacement" ? value : pair.replacement}`;
    });
    void commitAdvanced({ ...advanced, fontSubstitutes: substitutions });
  };

  const addSubstitution = () => {
    if (fontFamilies.length < 2) return;
    const used = new Set(advanced.fontSubstitutes.map((mapping) => splitSubstitution(mapping).source.toLocaleLowerCase()));
    const source = fontFamilies.find((font) => !used.has(font.toLocaleLowerCase())) ?? fontFamilies[0];
    const replacement = fontFamilies.find((font) => font !== source && font === "Segoe UI")
      ?? fontFamilies.find((font) => font !== source)
      ?? source;
    void commitAdvanced({ ...advanced, fontSubstitutes: [...advanced.fontSubstitutes, `${source}=${replacement}`] });
  };

  const removeSubstitution = (index: number) => {
    void commitAdvanced({ ...advanced, fontSubstitutes: advanced.fontSubstitutes.filter((_, currentIndex) => currentIndex !== index) });
  };

  const fontListEntries = (kind: "excludeFonts" | "includeFonts") => (listDrafts[kind] ?? "").split(/\r?\n/).map((entry) => entry.trim()).filter(Boolean);

  const updateFontList = async (kind: "excludeFonts" | "includeFonts", entries: ReadonlyArray<string>) => {
    setListDrafts((current) => ({ ...current, [kind]: entries.join("\n") }));
    try {
      const snapshot = await updateProfileList(kind, entries);
      if (snapshot) setProfile(snapshot);
    } catch (error: unknown) {
      setPreviewError(error instanceof Error ? error.message : String(error));
    }
  };

  const updateVector = (values: ReadonlyArray<number>, index: number, value: number) => values.map((current, currentIndex) => currentIndex === index ? value : current);
  const colorValue = (value: number) => `#${value.toString(16).padStart(6, "0")}`;

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
        <div className="header-actions profile-actions">
          <select aria-label={t("profiles.select")} disabled={profiles.length === 0} onChange={(event) => void chooseProfile(event.target.value)} value={profile?.path ?? ""}>
            {profiles.map((entry) => <option key={entry.path} value={entry.path}>{entry.name}</option>)}
          </select>
          <input aria-label={t("profiles.copyName")} onChange={(event) => setCopyName(event.target.value)} placeholder={t("profiles.newName")} value={copyName} />
          <button className="button secondary" disabled={!profile || !copyName.trim()} onClick={() => void duplicate()} type="button"><CopyPlus aria-hidden="true" size={16} /> {t("profiles.duplicate")}</button>
          <button className="button primary" disabled={!profile || dirtyCount === 0 || saving} onClick={() => void save()} type="button"><Save aria-hidden="true" size={17} /> {saving ? t("profiles.saving") : t("profiles.save")}</button>
          <button className="button primary" disabled={!profile || applying} onClick={() => void apply()} type="button"><Play aria-hidden="true" size={17} /> {applying ? t("profiles.applying") : t("profiles.apply")}</button>
        </div>
      </header>

      {appliedProfile && <p aria-live="polite" className="success-message profile-apply-message" data-operation="apply-profile">{t("profiles.applied", { name: appliedProfile.split(/[\\/]/).pop() ?? appliedProfile })}</p>}

      <div className="profile-layout">
        <aside className="settings-index" aria-label={t("profiles.sections")}>
          <label className="search-field"><Search aria-hidden="true" size={16} /><span className="sr-only">{t("profiles.search")}</span><input onChange={(event) => setQuery(event.target.value)} placeholder={t("profiles.search")} type="search" value={query} /></label>
          <ul>{groups.map((group) => <li key={group.id}><button data-selected={!query && activeGroup === group.id} onClick={() => { setActiveGroup(group.id); setQuery(""); }} type="button">{group.label}</button></li>)}</ul>
          <label className="checkbox-row"><input checked={showAdvanced} onChange={(event) => setShowAdvanced(event.target.checked)} type="checkbox" /> {t("profiles.showAdvanced")}</label>
        </aside>

        <div className="settings-workspace">
          <div className="settings-form">
            <div className="section-heading"><div><h2>{query ? t("profiles.searchResults") : activeDefinition.label}</h2><p>{query ? t("profiles.searchDescription", { query }) : activeDefinition.description}</p></div></div>

            {(query || activeGroup === "basic" || activeGroup === "shape" || activeGroup === "lcd" || activeGroup === "advanced") && filteredSettings.map((setting) => {
              const value = values[setting.id] ?? setting.default;
              const dirty = profile?.dirtyKeys.includes(setting.id) ?? false;
              const settingLabel = t(settingMessageKey(setting.id, "label"));
              const settingDescription = t(settingMessageKey(setting.id, "description"));
              return (
                <div className="setting-row" key={setting.id}>
                  <div><label htmlFor={setting.id}>{settingLabel} {dirty && <span className="dirty-mark">{t("profiles.changed")}</span>}</label><p>{settingDescription} {t("profiles.settingMeta", { default: setting.default, min: setting.min, max: setting.max })}{setting.apply === "restart_required" ? ` · ${t("profiles.restartRequired")}` : ""}</p></div>
                  <div className="range-control">
                    {setting.control === "select" && "options" in setting ? (
                      <select id={setting.id} onChange={(event) => changeSetting(setting.id, Number(event.target.value))} value={value}>{setting.options.map((option) => <option key={option.value} value={option.value}>{t(settingOptionMessageKey(setting.id, option.value))}</option>)}</select>
                    ) : setting.control === "boolean" ? (
                      <label className="switch-control"><input checked={value === 1} id={setting.id} onChange={(event) => changeSetting(setting.id, event.target.checked ? 1 : 0)} type="checkbox" /><span>{value === 1 ? t("profiles.enabled") : t("profiles.disabled")}</span></label>
                    ) : (
                      <input id={setting.id} max={setting.max} min={setting.min} onChange={(event) => changeSetting(setting.id, Number(event.target.value))} step={setting.type === "integer" ? 1 : 0.01} type="range" value={value} />
                    )}
                    <output htmlFor={setting.id}>{value}{setting.unit === "px" ? " px" : ""}</output>
                    <button className="icon-button" aria-label={t("profiles.reset", { setting: settingLabel })} onClick={() => changeSetting(setting.id, setting.default)} type="button"><RotateCcw aria-hidden="true" size={15} /></button>
                  </div>
                </div>
              );
            })}

            {!query && activeGroup === "advanced" && (
              <div className="advanced-editor">
                <fieldset>
                  <legend>{t("advanced.shadow")}</legend><p>{t("advanced.shadowHelp")}</p>
                  <label className="checkbox-row"><input checked={advanced.shadow !== null} onChange={(event) => void commitAdvanced({ ...advanced, shadow: event.target.checked ? { offsetX: 1, offsetY: 1, darkAlpha: 4, darkColor: 0, lightAlpha: 4, lightColor: 0 } : null })} type="checkbox" /> {t("advanced.enableCustom")}</label>
                  {advanced.shadow && <div className="advanced-grid">{(["offsetX", "offsetY", "darkAlpha", "lightAlpha"] as const).map((key) => <label key={key}><span>{t(`advanced.${key}`)}</span><input max={key.includes("Alpha") ? 255 : 128} min={key.includes("Alpha") ? 0 : -128} onChange={(event) => setAdvanced({ ...advanced, shadow: { ...advanced.shadow!, [key]: Number(event.target.value) } })} onBlur={() => void commitAdvanced(advanced)} type="number" value={advanced.shadow?.[key] ?? 0} /></label>)}{(["darkColor", "lightColor"] as const).map((key) => <label key={key}><span>{t(`advanced.${key}`)}</span><input onChange={(event) => void commitAdvanced({ ...advanced, shadow: { ...advanced.shadow!, [key]: Number.parseInt(event.target.value.slice(1), 16) } })} type="color" value={colorValue(advanced.shadow?.[key] ?? 0)} /></label>)}</div>}
                </fieldset>
                <fieldset>
                  <legend>{t("advanced.lcdWeight")}</legend><p>{t("advanced.lcdWeightHelp")}</p>
                  <label className="checkbox-row"><input checked={advanced.lcdFilterWeight !== null} onChange={(event) => void commitAdvanced({ ...advanced, lcdFilterWeight: event.target.checked ? [8, 77, 86, 77, 8] : null })} type="checkbox" /> {t("advanced.enableCustom")}</label>
                  {advanced.lcdFilterWeight && <div className="vector-grid">{advanced.lcdFilterWeight.map((value, index) => <label key={index}><span>{index + 1}</span><input max={255} min={0} onChange={(event) => setAdvanced({ ...advanced, lcdFilterWeight: updateVector(advanced.lcdFilterWeight!, index, Number(event.target.value)) })} onBlur={() => void commitAdvanced(advanced)} type="number" value={value} /></label>)}</div>}
                </fieldset>
                <fieldset>
                  <legend>{t("advanced.pixelLayout")}</legend><p>{t("advanced.pixelLayoutHelp")}</p>
                  <label className="checkbox-row"><input checked={advanced.pixelLayout !== null} onChange={(event) => void commitAdvanced({ ...advanced, pixelLayout: event.target.checked ? [-21, 0, 0, 0, 21, 0] : null })} type="checkbox" /> {t("advanced.enableCustom")}</label>
                  {advanced.pixelLayout && <div className="vector-grid">{advanced.pixelLayout.map((value, index) => <label key={index}><span>{["R x", "R y", "G x", "G y", "B x", "B y"][index]}</span><input max={127} min={-128} onChange={(event) => setAdvanced({ ...advanced, pixelLayout: updateVector(advanced.pixelLayout!, index, Number(event.target.value)) })} onBlur={() => void commitAdvanced(advanced)} type="number" value={value} /></label>)}</div>}
                </fieldset>
                <fieldset className="advanced-text-fields">
                  <legend>{t("advanced.routing")}</legend>
                  <label><span>{t("advanced.displayAffinity")}</span><small>{t("advanced.displayAffinityHelp")}</small><input onChange={(event) => setAdvanced({ ...advanced, displayAffinity: event.target.value.split(",").map((part) => Number(part.trim())).filter(Number.isInteger) })} onBlur={() => void commitAdvanced(advanced)} type="text" value={advanced.displayAffinity.join(", ")} /></label>
                  <div className="font-substitution-editor">
                    <strong>{t("advanced.fontSubstitutes")}</strong>
                    <small>{t("advanced.fontSubstitutesHelp")}</small>
                    <div className="font-substitution-list">
                      {advanced.fontSubstitutes.map((mapping, index) => {
                        const pair = splitSubstitution(mapping);
                        return (
                          <div className="font-substitution-row" key={`${mapping}-${index}`}>
                            <label><span className="sr-only">{t("profiles.sourceFont")}</span><select aria-label={t("profiles.sourceFont")} onChange={(event) => updateSubstitution(index, "source", event.target.value)} value={pair.source}>{fontFamilies.map((font) => <option key={font} value={font}>{fontOptionLabel(font)}</option>)}</select></label>
                            <ArrowRight aria-hidden="true" size={16} />
                            <label><span className="sr-only">{t("profiles.replacementFont")}</span><select aria-label={t("profiles.replacementFont")} onChange={(event) => updateSubstitution(index, "replacement", event.target.value)} value={pair.replacement}>{fontFamilies.map((font) => <option key={font} value={font}>{fontOptionLabel(font)}</option>)}</select></label>
                            <button className="icon-button" aria-label={t("profiles.remove", { name: pair.source })} onClick={() => removeSubstitution(index)} type="button"><Trash2 aria-hidden="true" size={15} /></button>
                          </div>
                        );
                      })}
                    </div>
                    <button className="button secondary font-add-button" disabled={fontFamilies.length < 2} onClick={addSubstitution} type="button"><Plus aria-hidden="true" size={16} /> {t("profiles.addSubstitution")}</button>
                  </div>
                </fieldset>
                <fieldset>
                  <legend>Infinality</legend>
                  <p>{t("advanced.infinalityVectorsHelp")}</p>
                  <div className="vector-grid">{advanced.infinalityGammaCorrection.map((value, index) => <label key={`gamma-${index}`}><span>{t("advanced.gammaCorrection")} {index + 1}</span><input onChange={(event) => setAdvanced({ ...advanced, infinalityGammaCorrection: updateVector(advanced.infinalityGammaCorrection, index, Number(event.target.value)) })} onBlur={() => void commitAdvanced(advanced)} type="number" value={value} /></label>)}{advanced.infinalityFilterParams.map((value, index) => <label key={`filter-${index}`}><span>{t("advanced.filterParams")} {index + 1}</span><input max={255} min={0} onChange={(event) => setAdvanced({ ...advanced, infinalityFilterParams: updateVector(advanced.infinalityFilterParams, index, Number(event.target.value)) })} onBlur={() => void commitAdvanced(advanced)} type="number" value={value} /></label>)}</div>
                </fieldset>
              </div>
            )}

            {!query && activeGroup === "individual" && (
              <div className="collection-editor">
                <div className="font-picker-row"><label htmlFor="individual-font-picker">{t("profiles.selectFont")}</label><select id="individual-font-picker" onChange={(event) => addIndividual(event.target.value)} value=""><option value="">{t("profiles.selectFont")}</option>{fontFamilies.filter((font) => installedFontKeys.has(font.toLocaleLowerCase()) && !individuals.some((entry) => entry.fontFace.toLocaleLowerCase() === font.toLocaleLowerCase())).map((font) => <option key={font} value={font}>{font}</option>)}</select></div>
                {individuals.map((entry, rowIndex) => (
                  <div className="individual-row" key={`${entry.fontFace}-${rowIndex}`}>
                    <strong>{entry.fontFace}</strong>
                    <div>{individualLabels.map((label, valueIndex) => <label key={label}><span>{label}</span><input aria-label={`${entry.fontFace} ${label}`} max={valueIndex === 2 ? 64 : valueIndex === 3 || valueIndex === 4 ? 32 : valueIndex === 1 ? 6 : valueIndex === 0 ? 2 : 1} min={valueIndex === 2 ? -64 : valueIndex === 3 || valueIndex === 4 ? -32 : valueIndex === 1 ? -1 : 0} onChange={(event) => { const next = individuals.map((item) => ({ ...item, values: [...item.values] })); next[rowIndex].values[valueIndex] = event.target.value === "" ? null : Number(event.target.value); void commitIndividuals(next); }} placeholder={t("profiles.inherit")} type="number" value={entry.values[valueIndex] ?? ""} /></label>)}</div>
                    <button className="icon-button" aria-label={t("profiles.remove", { name: entry.fontFace })} onClick={() => void commitIndividuals(individuals.filter((_, index) => index !== rowIndex))} type="button"><Trash2 aria-hidden="true" size={15} /></button>
                  </div>
                ))}
                {individuals.length === 0 && <p className="empty-state">{t("profiles.emptyIndividuals")}</p>}
              </div>
            )}

            {!query && activeGroup === "lists" && <div className="list-grid">{listDefinitions.map((definition) => {
              if (definition.kind === "excludeFonts" || definition.kind === "includeFonts") {
                const entries = fontListEntries(definition.kind);
                const available = fontFamilies.filter((font) => installedFontKeys.has(font.toLocaleLowerCase()) && !entries.some((entry) => entry.toLocaleLowerCase() === font.toLocaleLowerCase()));
                return (
                  <section className="font-list-editor" key={definition.kind}>
                    <strong>{definition.label}</strong>
                    <span>{definition.help}</span>
                    <ul>{entries.map((font) => <li key={font}><span>{fontOptionLabel(font)}</span><button className="icon-button" aria-label={t("profiles.remove", { name: font })} onClick={() => void updateFontList(definition.kind, entries.filter((entry) => entry !== font))} type="button"><Trash2 aria-hidden="true" size={14} /></button></li>)}</ul>
                    <select aria-label={`${definition.label} · ${t("profiles.addFontToList")}`} disabled={available.length === 0} onChange={(event) => { if (event.target.value) void updateFontList(definition.kind, [...entries, event.target.value]); }} value=""><option value="">{t("profiles.addFontToList")}</option>{available.map((font) => <option key={font} value={font}>{font}</option>)}</select>
                  </section>
                );
              }
              return <label key={definition.kind}><strong>{definition.label}</strong><span>{definition.help}</span><textarea onBlur={() => void commitList(definition.kind)} onChange={(event) => setListDrafts((current) => ({ ...current, [definition.kind]: event.target.value }))} rows={6} value={listDrafts[definition.kind] ?? ""} /></label>;
            })}</div>}
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
