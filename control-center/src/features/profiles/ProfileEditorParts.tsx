import { useI18n } from "../../i18n/i18n";
import { BadgeCheck, ListRestart, Play, Redo2, RotateCcw, Save, SaveAll, Undo2, Upload } from "lucide-react";
import { ProfileNameDialog } from "../../components/ProfileNameDialog";
import { Hint } from "../../components/Hint";
import { settingsSchema } from "../../generated/settings";
import { AdvancedSettings } from "./AdvancedSettings";
import { IndividualSettings } from "./IndividualSettings";
import { ListsEditor } from "./ListsEditor";
import { ProfilePreviewPanel } from "./ProfilePreviewPanel";
import { BasicSettings, LcdSettings, SearchSettings, ShapeSettings } from "./SchemaSettings";
import { GuidedSettings } from "./GuidedSettings";
import type { ProfileEditor } from "./useProfileEditor";

interface EditorPartProps {
  editor: ProfileEditor;
}

/* The heading above the step or group body. Skins place it inside their own
   panel or card, so it carries no page-level chrome. */
export function ProfileEditorHeading({ editor }: EditorPartProps) {
  return <h2><Hint content={editor.editing.headingHint}>{editor.editing.headingText}</Hint></h2>;
}

/* The settings body: guided step contents, or one settings group, or the
   search results. Shared by every skin; only the surrounding chrome differs. */
export function ProfileEditorBody({ editor }: EditorPartProps) {
  const { t } = useI18n();
  const { mode, query, activeGroup } = editor.editing;
  return (
    <>
      {mode === "guided" && <GuidedSettings activeStep={editor.editing.activeGuidedStep} advanced={editor.document.advanced} busy={editor.editing.guidedBusy} canRedoStep={editor.history.stepHistory.canRedo(editor.editing.activeGuidedStep)} canSave={editor.document.profile?.canSave ?? false} canUndoStep={editor.history.stepHistory.canUndo(editor.editing.activeGuidedStep)} dirtyCount={editor.document.dirtyCount} dirtyKeys={editor.document.dirtyKeys} fontFace={editor.preview.fontFace} fontFamilies={editor.preview.fontFamilies} fontOptionLabel={editor.preview.fontOptionLabel} onAdvancedCommit={(next) => void editor.document.commitAdvanced(next)} onApply={(intent) => void editor.document.designateProfile(intent)} designating={editor.document.command === "designate"} starting={editor.document.command === "start"} followUp={editor.document.followUp} offerStart={editor.document.offerStart} onSaveAs={() => editor.files.setNameDialogOpen(true)} onStartService={() => void editor.document.startServiceNow()} onFontFaceChange={editor.preview.setFontFace} onPreview={editor.preview.showPreview} onRedoStep={editor.history.redoStepEdit} onSave={() => void editor.document.saveCurrentProfile()} onSettingChange={editor.editing.changeGuidedSetting} onSettingPreview={editor.document.previewSetting} onStepChange={editor.editing.setActiveGuidedStep} onUndoStep={editor.history.undoStepEdit} profileName={editor.document.profile?.displayPath ?? null} profilePath={editor.document.profile?.path ?? null} savedValues={editor.document.savedValues} settings={settingsSchema} t={t} values={editor.document.values} />}

      {mode === "all" && query && <SearchSettings dirtyKeys={editor.document.dirtyKeys} onChange={editor.document.changeSetting} onPreviewChange={editor.document.previewSetting} savedValues={editor.document.savedValues} settings={editor.editing.filteredSettings} t={t} values={editor.document.values} />}
      {mode === "all" && !query && activeGroup === "basic" && <BasicSettings dirtyKeys={editor.document.dirtyKeys} onChange={editor.document.changeSetting} onPreviewChange={editor.document.previewSetting} savedValues={editor.document.savedValues} settings={editor.editing.filteredSettings} t={t} values={editor.document.values} />}
      {mode === "all" && !query && activeGroup === "shape" && <ShapeSettings dirtyKeys={editor.document.dirtyKeys} onChange={editor.document.changeSetting} onPreviewChange={editor.document.previewSetting} savedValues={editor.document.savedValues} settings={editor.editing.filteredSettings} t={t} values={editor.document.values} />}
      {mode === "all" && !query && activeGroup === "lcd" && <LcdSettings dirtyKeys={editor.document.dirtyKeys} onChange={editor.document.changeSetting} onPreviewChange={editor.document.previewSetting} savedValues={editor.document.savedValues} settings={editor.editing.filteredSettings} t={t} values={editor.document.values} />}

      {mode === "all" && !query && activeGroup === "advanced" && (
        <AdvancedSettings
          advanced={editor.document.advanced}
          dirtyKeys={editor.document.dirtyKeys}
          fontFamilies={editor.preview.fontFamilies}
          fontOptionLabel={editor.preview.fontOptionLabel}
          onAdvancedChange={editor.document.setAdvanced}
          onAdvancedCommit={(next) => void editor.document.commitAdvanced(next)}
          onOpenList={(kind) => editor.editing.chooseGroup("lists", kind)}
          onSettingChange={editor.document.changeSetting}
          onSettingPreview={editor.document.previewSetting}
          onUnityGamesChange={(games) => void editor.document.updateList("unityIncludeGames", games)}
          savedValues={editor.document.savedValues}
          settings={editor.editing.filteredSettings}
          t={t}
          unityGames={editor.document.lists.unityIncludeGames ?? []}
          values={editor.document.values}
        />
      )}

      {mode === "all" && !query && activeGroup === "individual" && (
        <IndividualSettings
          fontFamilies={editor.preview.fontFamilies}
          individualLabels={editor.editing.individualLabels}
          individuals={editor.document.individuals}
          installedFontKeys={editor.preview.installedFontKeys}
          onAdd={editor.document.addIndividual}
          onCommit={(next) => void editor.document.commitIndividuals(next)}
          t={t}
        />
      )}

      {mode === "all" && !query && activeGroup === "lists" && (
        <ListsEditor
          definitions={editor.editing.listDefinitions}
          entries={editor.document.lists}
          focusKind={editor.editing.listFocus}
          fontFamilies={editor.preview.fontFamilies}
          fontOptionLabel={editor.preview.fontOptionLabel}
          installedFontKeys={editor.preview.installedFontKeys}
          onFocusHandled={editor.editing.clearListFocus}
          onUpdateList={(kind, entries) => void editor.document.updateList(kind, entries)}
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
  const { t } = useI18n();
  return (
    <ProfilePreviewPanel
      ciSmoke={ciSmoke}
      docked={editor.preview.previewDocked}
      error={editor.document.error}
      fontFace={editor.preview.fontFace}
      fontFamilies={editor.preview.fontFamilies}
      fontOptionLabel={editor.preview.fontOptionLabel}
      mode={editor.editing.mode}
      onError={editor.preview.setPreviewError}
      onFontFaceChange={editor.preview.setFontFace}
      onOpenStudio={onOpenStudio}
      onPreviewReady={onPreviewReady}
      profilePath={editor.document.profile?.path ?? null}
      ref={editor.preview.previewPanelRef}
      savedValues={editor.document.savedValues}
      t={t}
      values={editor.document.values}
      variants={editor.preview.previewVariants}
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
  const { t } = useI18n();
  const { profile, busy, dirtyCount, recoveryRequired, command, followUp, offerStart } = editor.document;
  if (editor.editing.mode !== "all") return null;
  const icons = variant === "icons";
  const undoLabel = t("profiles.undo");
  const redoLabel = t("profiles.redo");
  return (
    <div aria-label={t("profiles.editActions")} className={className ?? "profile-history-actions"} role="toolbar">
      <button aria-label={icons ? undoLabel : undefined} className={icons ? "icon-button" : "button secondary compact-action"} disabled={!profile?.canUndo || busy} onClick={() => void editor.document.undo()} title={icons ? undoLabel : undefined} type="button"><Undo2 aria-hidden="true" size={14} />{!icons && <> {undoLabel}</>}</button>
      <button aria-label={icons ? redoLabel : undefined} className={icons ? "icon-button" : "button secondary compact-action"} disabled={!profile?.canRedo || busy} onClick={() => void editor.document.redo()} title={icons ? redoLabel : undefined} type="button"><Redo2 aria-hidden="true" size={14} />{!icons && <> {redoLabel}</>}</button>
      <button className="button secondary compact-action" disabled={!profile || dirtyCount === 0 || busy} onClick={() => void editor.document.discard()} title={t("profiles.discardDescription")} type="button"><RotateCcw aria-hidden="true" size={14} /> {t("profiles.discard")}</button>
      <button className="button secondary compact-action" disabled={!profile || busy || recoveryRequired} onClick={editor.document.resetDefaults} title={t("profiles.resetDefaultsDescription")} type="button"><ListRestart aria-hidden="true" size={14} /> {t("profiles.resetDefaults")}</button>
      <button className="button secondary compact-action" disabled={!profile || !profile.canSave || dirtyCount === 0 || busy || recoveryRequired} data-dirty={dirtyCount > 0} onClick={() => void editor.document.saveCurrentProfile()} type="button"><Save aria-hidden="true" size={14} /> {command === "save" ? t("profiles.saving") : t("profiles.save")}</button>
      <button className="button secondary compact-action" disabled={!profile || busy || recoveryRequired} onClick={() => editor.files.setNameDialogOpen(true)} type="button"><SaveAll aria-hidden="true" size={14} /> {t("files.saveAs")}</button>
      {followUp === "designate" && <button className="button designate compact-action" disabled={!profile || dirtyCount > 0 || busy || recoveryRequired} onClick={() => void editor.document.designateProfile()} title={dirtyCount > 0 ? t("profiles.saveBeforeDesignate") : undefined} type="button"><BadgeCheck aria-hidden="true" size={14} /> {command === "designate" ? t("profiles.designating") : t("profiles.designate")}</button>}
      {followUp === "apply-to-service" && <button className="button designate compact-action" disabled={!profile || dirtyCount > 0 || busy || recoveryRequired} onClick={() => void editor.document.designateProfile("apply-to-service")} title={t("profiles.applyToServiceDescription")} type="button"><Upload aria-hidden="true" size={14} /> {command === "designate" ? t("profiles.applyingToService") : t("profiles.applyToService")}</button>}
      {!followUp && offerStart && <button className="button designate compact-action" disabled={busy} onClick={() => void editor.document.startServiceNow()} type="button"><Play aria-hidden="true" size={14} /> {command === "start" ? t("execution.serviceWorking") : t("files.startServiceNow")}</button>}
    </div>
  );
}

export function ProfileEditorNameDialog({ editor }: EditorPartProps) {
  if (!editor.files.nameDialogOpen) return null;
  return <ProfileNameDialog busy={editor.document.busy} initialName={editor.document.suggestedProfileName} onCancel={() => editor.files.setNameDialogOpen(false)} onSubmit={editor.files.submitSaveAs} verdict={editor.document.profileNameVerdict} />;
}

/* The one-line document summary under a page title: which file, how many
   unsaved changes, and the read-only warning when the original cannot be
   saved. */
export function ProfileEditorSummary({ editor }: EditorPartProps) {
  const { t } = useI18n();
  const { loading, profile, dirtyCount, message } = editor.document;
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
