import { Play, Redo2, RotateCcw, Save, SaveAll, Search, Undo2, X } from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { settingsSchema } from "../generated/settings";
import { settingMessageKey, useI18n } from "../i18n/i18n";
import { loadInstalledFontFamilies } from "../app/tauri";
import { AdvancedSettings } from "./profiles/AdvancedSettings";
import { IndividualSettings } from "./profiles/IndividualSettings";
import { ListsEditor } from "./profiles/ListsEditor";
import { BasicSettings, LcdSettings, SearchSettings, ShapeSettings } from "./profiles/SchemaSettings";
import { splitSubstitution } from "./profiles/profileEditorUtils";
import { ProfilePreviewPanel, type ProfilePreviewHandle } from "./profiles/ProfilePreviewPanel";
import { useProfileDocument } from "./profiles/useProfileDocument";
import { WizardSettings } from "./profiles/WizardSettings";
import { wizardStepIds, type WizardStepId } from "./profiles/wizardModel";

type GroupId = "basic" | "shape" | "lcd" | "advanced" | "individual" | "lists";
type ProfileMode = "quick" | "advanced";

interface ProfilesPageProps {
  ciSmoke?: boolean;
  mode?: ProfileMode;
  onModeChange?: (mode: ProfileMode) => void;
  onPreviewReady?: () => void;
}

export function ProfilesPage({ ciSmoke = false, mode = "advanced", onPreviewReady }: ProfilesPageProps) {
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
  const {
    addIndividual,
    advanced,
    applyProfile,
    busy,
    changeSetting,
    command: profileCommand,
    commitAdvanced,
    commitIndividuals,
    commitList,
    dirtyCount,
    dirtyKeys,
    discard,
    error: previewError,
    individuals,
    listDrafts,
    loading,
    message: profileMessage,
    previewSetting,
    profile,
    recoveryRequired,
    redo,
    saveCurrentProfile,
    saveProfileAs,
    setAdvanced,
    setError: setPreviewError,
    undo,
    updateFontList,
    updateListDraft,
    values,
  } = useProfileDocument(t);
  const [activeGroup, setActiveGroup] = useState<GroupId>("basic");
  const [activeWizardStep, setActiveWizardStep] = useState<WizardStepId>("rendering");
  const [installedFonts, setInstalledFonts] = useState<ReadonlyArray<string>>([]);
  const [fontFace, setFontFace] = useState("Segoe UI");
  const [query, setQuery] = useState("");
  const [saveAsOpen, setSaveAsOpen] = useState(false);
  const [saveAsName, setSaveAsName] = useState("");
  const previewPanelRef = useRef<ProfilePreviewHandle>(null);

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
  }, [setPreviewError]);

  const filteredSettings = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    return settingsSchema.filter((setting) => {
      if (!needle && setting.group !== activeGroup) return false;
      const localized = `${t(settingMessageKey(setting.id, "label"))} ${t(settingMessageKey(setting.id, "description"))} ${setting.key}`;
      return !needle || localized.toLocaleLowerCase().includes(needle);
    });
  }, [activeGroup, query, t]);

  const showPreview = () => {
    previewPanelRef.current?.show();
  };

  const activeDefinition = groups.find((group) => group.id === activeGroup) ?? groups[0];
  const activeWizardLabel = t(`wizard.${activeWizardStep}`);
  return (
    <section className="page profile-page view-enter" aria-labelledby="profiles-title" data-mode={mode}>
      <header className="page-header compact profile-header">
        <div>
          <div className="profile-mode-title"><h1 id="profiles-title">{t(mode === "quick" ? "nav.quickSetup" : "nav.advancedTuning")}</h1><span>{mode === "quick" ? "Wizard" : "Tuner"}</span></div>
          <p>{t(mode === "quick" ? "profiles.quickDescription" : "profiles.advancedDescription")}</p>
          {loading
            ? <p>{t("profiles.searching")}</p>
            : <p className="profile-editing"><span>{t("profiles.editing")}</span> <code title={profile?.path}>{profile?.displayPath ?? t("profiles.none")}</code><span> · {t("profiles.unsavedSummary", { count: dirtyCount })}</span></p>}
          {profile && !profile.canSave && <p className="profile-save-warning">{t("profiles.readOnly")}</p>}
          {profileMessage && <p aria-live="polite" className="profile-message">{profileMessage}</p>}
        </div>
        <div aria-label={t("profiles.editActions")} className="profile-history-actions" role="toolbar">
          <button className="button secondary compact-action" disabled={!profile?.canUndo || busy} onClick={() => void undo()} type="button"><Undo2 aria-hidden="true" size={16} /> {t("profiles.undo")}</button>
          <button className="button secondary compact-action" disabled={!profile?.canRedo || busy} onClick={() => void redo()} type="button"><Redo2 aria-hidden="true" size={16} /> {t("profiles.redo")}</button>
          <button className="button secondary compact-action" disabled={!profile || dirtyCount === 0 || busy} onClick={() => void discard()} title={t("profiles.discardDescription")} type="button"><RotateCcw aria-hidden="true" size={16} /> {t("profiles.discard")}</button>
          <button className="button secondary compact-action" disabled={!profile || !profile.canSave || dirtyCount === 0 || busy || recoveryRequired} onClick={() => void saveCurrentProfile()} type="button"><Save aria-hidden="true" size={16} /> {profileCommand === "save" ? t("profiles.saving") : t("profiles.saveNow")}</button>
          {profile && !profile.canSave && <button className="button secondary compact-action" disabled={busy || recoveryRequired} onClick={() => setSaveAsOpen(true)} type="button"><SaveAll aria-hidden="true" size={16} /> {t("files.saveAs")}</button>}
          <button className="button primary compact-action" disabled={!profile || dirtyCount > 0 || busy || recoveryRequired} onClick={() => void applyProfile()} title={dirtyCount > 0 ? t("profiles.saveBeforeApply") : undefined} type="button"><Play aria-hidden="true" size={16} /> {profileCommand === "apply" ? t("profiles.applying") : t("profiles.applyNow")}</button>
        </div>
      </header>

      {saveAsOpen && (
        <form className="profile-save-as" onSubmit={(event) => {
          event.preventDefault();
          void saveProfileAs(saveAsName).then((saved) => {
            if (saved) {
              setSaveAsName("");
              setSaveAsOpen(false);
            }
          });
        }}>
          <label><span>{t("profiles.saveAsName")}</span><input autoFocus disabled={busy} onChange={(event) => setSaveAsName(event.target.value)} value={saveAsName} /></label>
          <button className="button primary" disabled={busy || !saveAsName.trim()} type="submit"><SaveAll aria-hidden="true" size={16} /> {profileCommand === "save-as" ? t("profiles.saving") : t("files.saveAs")}</button>
          <button aria-label={t("profiles.cancelSaveAs")} className="icon-button" disabled={busy} onClick={() => setSaveAsOpen(false)} title={t("profiles.cancelSaveAs")} type="button"><X aria-hidden="true" size={16} /></button>
        </form>
      )}

      <div className="profile-layout">
        <aside className="settings-index" aria-label={mode === "quick" ? t("wizard.progress") : t("profiles.sections")}>
          {mode === "advanced" && <label className="search-field"><Search aria-hidden="true" size={16} /><span className="sr-only">{t("profiles.search")}</span><input onChange={(event) => setQuery(event.target.value)} placeholder={t("profiles.search")} type="search" value={query} /></label>}
          <ul>{mode === "quick" ? wizardStepIds.map((step, index) => <li key={step}><button data-selected={activeWizardStep === step} onClick={() => setActiveWizardStep(step)} type="button"><span className="settings-step" aria-hidden="true">{index + 1}</span><span>{t(`wizard.${step}`)}</span></button></li>) : groups.map((group) => <li key={group.id}><button data-selected={!query && activeGroup === group.id} onClick={() => { setActiveGroup(group.id); setQuery(""); }} type="button"><span>{group.label}</span></button></li>)}</ul>
        </aside>

        <div className="settings-workspace">
          <div className="settings-form">
            <div className="section-heading"><div><h2>{mode === "quick" ? activeWizardLabel : query ? t("profiles.searchResults") : activeDefinition.label}</h2><p>{mode === "quick" ? t("wizard.guidance") : query ? t("profiles.searchDescription", { query }) : activeDefinition.description}</p></div></div>

            {mode === "quick" && <WizardSettings activeStep={activeWizardStep} advanced={advanced} busy={!profile || busy || recoveryRequired} canSave={profile?.canSave ?? false} dirtyCount={dirtyCount} dirtyKeys={dirtyKeys} fontFamilies={fontFamilies} fontOptionLabel={fontOptionLabel} onAdvancedCommit={(next) => void commitAdvanced(next)} onApply={() => void applyProfile()} onPreview={showPreview} onSave={() => void saveCurrentProfile()} onSettingChange={changeSetting} onSettingPreview={previewSetting} onStepChange={setActiveWizardStep} settings={settingsSchema} t={t} values={values} />}

            {mode === "advanced" && query && <SearchSettings dirtyKeys={dirtyKeys} onChange={changeSetting} onPreviewChange={previewSetting} settings={filteredSettings} t={t} values={values} />}
            {mode === "advanced" && !query && activeGroup === "basic" && <BasicSettings dirtyKeys={dirtyKeys} onChange={changeSetting} onPreviewChange={previewSetting} settings={filteredSettings} t={t} values={values} />}
            {mode === "advanced" && !query && activeGroup === "shape" && <ShapeSettings dirtyKeys={dirtyKeys} onChange={changeSetting} onPreviewChange={previewSetting} settings={filteredSettings} t={t} values={values} />}
            {mode === "advanced" && !query && activeGroup === "lcd" && <LcdSettings dirtyKeys={dirtyKeys} onChange={changeSetting} onPreviewChange={previewSetting} settings={filteredSettings} t={t} values={values} />}

            {mode === "advanced" && !query && activeGroup === "advanced" && (
              <AdvancedSettings
                advanced={advanced}
                dirtyKeys={dirtyKeys}
                fontFamilies={fontFamilies}
                fontOptionLabel={fontOptionLabel}
                onAdvancedChange={setAdvanced}
                onAdvancedCommit={(next) => void commitAdvanced(next)}
                onSettingChange={changeSetting}
                onSettingPreview={previewSetting}
                settings={filteredSettings}
                t={t}
                values={values}
              />
            )}

            {mode === "advanced" && !query && activeGroup === "individual" && (
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

            {mode === "advanced" && !query && activeGroup === "lists" && (
              <ListsEditor
                definitions={listDefinitions}
                drafts={listDrafts}
                fontFamilies={fontFamilies}
                fontOptionLabel={fontOptionLabel}
                installedFontKeys={installedFontKeys}
                onCommit={(kind) => void commitList(kind)}
                onDraftChange={updateListDraft}
                onUpdateFontList={(kind, entries) => void updateFontList(kind, entries)}
                t={t}
              />
            )}
          </div>

          <ProfilePreviewPanel
            ciSmoke={ciSmoke}
            error={previewError}
            fontFace={fontFace}
            fontFamilies={fontFamilies}
            fontOptionLabel={fontOptionLabel}
            mode={mode}
            onError={setPreviewError}
            onFontFaceChange={setFontFace}
            onPreviewReady={onPreviewReady}
            profilePath={profile?.path ?? null}
            ref={previewPanelRef}
            t={t}
            values={values}
          />
        </div>
      </div>
    </section>
  );
}
