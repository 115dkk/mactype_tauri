import { ListRestart, Play, Redo2, RotateCcw, Save, SaveAll, Undo2, X } from "lucide-react";
import { Hint } from "../../components/Hint";
import { settingsSchema } from "../../generated/settings";
import { AdvancedSettings } from "../../pages/profiles/AdvancedSettings";
import { IndividualSettings } from "../../pages/profiles/IndividualSettings";
import { ListsEditor } from "../../pages/profiles/ListsEditor";
import { ProfilePreviewPanel } from "../../pages/profiles/ProfilePreviewPanel";
import { BasicSettings, LcdSettings, SearchSettings, ShapeSettings } from "../../pages/profiles/SchemaSettings";
import { WizardSettings } from "../../pages/profiles/WizardSettings";
import type { ProfileEditor } from "./useProfileEditor";

interface EditorPartProps {
  editor: ProfileEditor;
}

/* The heading above the step or group body. Skins place it inside their own
   panel or card, so it carries no page-level chrome. */
export function ProfileEditorHeading({ editor }: EditorPartProps) {
  return <h2><Hint content={editor.headingHint}>{editor.headingText}</Hint></h2>;
}

/* The settings body: guided step contents, or one settings group, or the
   search results. Shared by every skin; only the surrounding chrome differs. */
export function ProfileEditorBody({ editor }: EditorPartProps) {
  const { mode, query, activeGroup, t } = editor;
  return (
    <>
      {mode === "quick" && <WizardSettings activeStep={editor.activeWizardStep} advanced={editor.advanced} busy={editor.guidedBusy} canRedoStep={editor.stepHistory.canRedo(editor.activeWizardStep)} canSave={editor.profile?.canSave ?? false} canUndoStep={editor.stepHistory.canUndo(editor.activeWizardStep)} dirtyCount={editor.dirtyCount} dirtyKeys={editor.dirtyKeys} fontFace={editor.fontFace} fontFamilies={editor.fontFamilies} fontOptionLabel={editor.fontOptionLabel} onAdvancedCommit={(next) => void editor.commitAdvanced(next)} onApply={() => void editor.applyProfile()} onFontFaceChange={editor.setFontFace} onPreview={editor.showPreview} onRedoStep={editor.redoStepEdit} onSave={() => void editor.saveCurrentProfile()} onSettingChange={editor.changeGuidedSetting} onSettingPreview={editor.previewSetting} onStepChange={editor.setActiveWizardStep} onUndoStep={editor.undoStepEdit} profileName={editor.profile?.displayPath ?? null} profilePath={editor.profile?.path ?? null} savedValues={editor.savedValues} settings={settingsSchema} t={t} values={editor.values} />}

      {mode === "advanced" && query && <SearchSettings dirtyKeys={editor.dirtyKeys} onChange={editor.changeSetting} onPreviewChange={editor.previewSetting} savedValues={editor.savedValues} settings={editor.filteredSettings} t={t} values={editor.values} />}
      {mode === "advanced" && !query && activeGroup === "basic" && <BasicSettings dirtyKeys={editor.dirtyKeys} onChange={editor.changeSetting} onPreviewChange={editor.previewSetting} savedValues={editor.savedValues} settings={editor.filteredSettings} t={t} values={editor.values} />}
      {mode === "advanced" && !query && activeGroup === "shape" && <ShapeSettings dirtyKeys={editor.dirtyKeys} onChange={editor.changeSetting} onPreviewChange={editor.previewSetting} savedValues={editor.savedValues} settings={editor.filteredSettings} t={t} values={editor.values} />}
      {mode === "advanced" && !query && activeGroup === "lcd" && <LcdSettings dirtyKeys={editor.dirtyKeys} onChange={editor.changeSetting} onPreviewChange={editor.previewSetting} savedValues={editor.savedValues} settings={editor.filteredSettings} t={t} values={editor.values} />}

      {mode === "advanced" && !query && activeGroup === "advanced" && (
        <AdvancedSettings
          advanced={editor.advanced}
          dirtyKeys={editor.dirtyKeys}
          fontFamilies={editor.fontFamilies}
          fontOptionLabel={editor.fontOptionLabel}
          onAdvancedChange={editor.setAdvanced}
          onAdvancedCommit={(next) => void editor.commitAdvanced(next)}
          onSettingChange={editor.changeSetting}
          onSettingPreview={editor.previewSetting}
          savedValues={editor.savedValues}
          settings={editor.filteredSettings}
          t={t}
          values={editor.values}
        />
      )}

      {mode === "advanced" && !query && activeGroup === "individual" && (
        <IndividualSettings
          fontFamilies={editor.fontFamilies}
          individualLabels={editor.individualLabels}
          individuals={editor.individuals}
          installedFontKeys={editor.installedFontKeys}
          onAdd={editor.addIndividual}
          onCommit={(next) => void editor.commitIndividuals(next)}
          t={t}
        />
      )}

      {mode === "advanced" && !query && activeGroup === "lists" && (
        <ListsEditor
          definitions={editor.listDefinitions}
          entries={editor.lists}
          fontFamilies={editor.fontFamilies}
          fontOptionLabel={editor.fontOptionLabel}
          installedFontKeys={editor.installedFontKeys}
          onUpdateList={(kind, entries) => void editor.updateList(kind, entries)}
          t={t}
        />
      )}
    </>
  );
}

interface PreviewPartProps extends EditorPartProps {
  ciSmoke: boolean;
  onPreviewReady?: () => void;
  onOpenStudio?: () => void;
}

export function ProfileEditorPreview({ editor, ciSmoke, onPreviewReady, onOpenStudio }: PreviewPartProps) {
  return (
    <ProfilePreviewPanel
      ciSmoke={ciSmoke}
      docked={editor.previewDocked}
      error={editor.error}
      fontFace={editor.fontFace}
      fontFamilies={editor.fontFamilies}
      fontOptionLabel={editor.fontOptionLabel}
      mode={editor.mode}
      onError={editor.setError}
      onFontFaceChange={editor.setFontFace}
      onOpenStudio={onOpenStudio}
      onPreviewReady={onPreviewReady}
      profilePath={editor.profile?.path ?? null}
      ref={editor.previewPanelRef}
      savedValues={editor.savedValues}
      t={editor.t}
      values={editor.values}
      variants={editor.previewVariants}
    />
  );
}

interface ToolbarProps extends EditorPartProps {
  /* "text" shows every label; "icons" shows undo and redo as icon buttons and
     keeps labels on the save and apply commands, which is what the denser
     skins draw in their command bars. */
  variant?: "text" | "icons";
  className?: string;
}

/* The document command set (undo, redo, discard, reset, save, save as, apply)
   with one enabling rule, so a skin cannot expose a command the document
   refuses. Advanced mode only; guided steps carry their own step tools. */
export function ProfileEditorToolbar({ editor, variant = "text", className }: ToolbarProps) {
  const { t, profile, busy, dirtyCount, recoveryRequired, command } = editor;
  if (editor.mode !== "advanced") return null;
  const icons = variant === "icons";
  const undoLabel = t("profiles.undo");
  const redoLabel = t("profiles.redo");
  return (
    <div aria-label={t("profiles.editActions")} className={className ?? "profile-history-actions"} role="toolbar">
      <button aria-label={icons ? undoLabel : undefined} className={icons ? "icon-button" : "button secondary compact-action"} disabled={!profile?.canUndo || busy} onClick={() => void editor.undo()} title={icons ? undoLabel : undefined} type="button"><Undo2 aria-hidden="true" size={14} />{!icons && <> {undoLabel}</>}</button>
      <button aria-label={icons ? redoLabel : undefined} className={icons ? "icon-button" : "button secondary compact-action"} disabled={!profile?.canRedo || busy} onClick={() => void editor.redo()} title={icons ? redoLabel : undefined} type="button"><Redo2 aria-hidden="true" size={14} />{!icons && <> {redoLabel}</>}</button>
      <button className="button secondary compact-action" disabled={!profile || dirtyCount === 0 || busy} onClick={() => void editor.discard()} title={t("profiles.discardDescription")} type="button"><RotateCcw aria-hidden="true" size={14} /> {t("profiles.discard")}</button>
      <button className="button secondary compact-action" disabled={!profile || busy || recoveryRequired} onClick={editor.resetDefaults} title={t("profiles.resetDefaultsDescription")} type="button"><ListRestart aria-hidden="true" size={14} /> {t("profiles.resetDefaults")}</button>
      <button className="button secondary compact-action" disabled={!profile || !profile.canSave || dirtyCount === 0 || busy || recoveryRequired} onClick={() => void editor.saveCurrentProfile()} type="button"><Save aria-hidden="true" size={14} /> {command === "save" ? t("profiles.saving") : t("profiles.saveNow")}</button>
      {profile && !profile.canSave && <button className="button secondary compact-action" disabled={busy || recoveryRequired} onClick={() => editor.setSaveAsOpen(true)} type="button"><SaveAll aria-hidden="true" size={14} /> {t("files.saveAs")}</button>}
      <button className="button primary compact-action" disabled={!profile || dirtyCount > 0 || busy || recoveryRequired} onClick={() => void editor.applyProfile()} title={dirtyCount > 0 ? t("profiles.saveBeforeApply") : undefined} type="button"><Play aria-hidden="true" size={14} /> {command === "apply" ? t("profiles.applying") : t("profiles.applyNow")}</button>
    </div>
  );
}

export function ProfileSaveAsForm({ editor }: EditorPartProps) {
  const { t, busy, saveAsName, command } = editor;
  if (!editor.saveAsOpen) return null;
  return (
    <form className="profile-save-as" onSubmit={(event) => { event.preventDefault(); editor.submitSaveAs(); }}>
      <label><span>{t("profiles.saveAsName")}</span><input autoFocus disabled={busy} onChange={(event) => editor.setSaveAsName(event.target.value)} value={saveAsName} /></label>
      <button className="button primary" disabled={busy || !saveAsName.trim()} type="submit"><SaveAll aria-hidden="true" size={16} /> {command === "save-as" ? t("profiles.saving") : t("files.saveAs")}</button>
      <button aria-label={t("profiles.cancelSaveAs")} className="icon-button" disabled={busy} onClick={() => editor.setSaveAsOpen(false)} title={t("profiles.cancelSaveAs")} type="button"><X aria-hidden="true" size={16} /></button>
    </form>
  );
}

/* The one-line document summary under a page title: which file, how many
   unsaved changes, and the read-only warning when the original cannot be
   saved. */
export function ProfileEditorSummary({ editor }: EditorPartProps) {
  const { t, loading, profile, dirtyCount, message } = editor;
  return (
    <>
      {loading
        ? <p>{t("profiles.searching")}</p>
        : <p className="profile-editing"><span>{t("profiles.editing")}</span> <code title={profile?.path}>{profile?.displayPath ?? t("profiles.none")}</code><span> · {t("profiles.unsavedSummary", { count: dirtyCount })}</span></p>}
      {profile && !profile.canSave && <p className="profile-save-warning">{t("profiles.readOnly")}</p>}
      {message && <p aria-live="polite" className="profile-message">{message}</p>}
    </>
  );
}
