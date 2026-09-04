import { Search } from "lucide-react";
import { StatusDot } from "../../components/StatusDot";
import { ProfileEditorBody, ProfileEditorPreview, ProfileEditorToolbar, ProfileSaveAsForm } from "../../features/profiles/ProfileEditorParts";
import { useProfileEditor } from "../../features/profiles/useProfileEditor";
import { useI18n } from "../../i18n/i18n";
import { ConsoleFrame, ConsolePanel } from "./ConsoleFrame";
import { useConsole } from "./consoleContext";

export function ConsoleTuner() {
  const { t } = useI18n();
  const { shell } = useConsole();
  const mode = shell.profileMode;
  const editor = useProfileEditor({ mode });
  const { profile, dirtyCount } = editor;
  const title = t(mode === "quick" ? "nav.guidedSetup" : "nav.allSettings");
  const profileName = profile?.displayPath.split(/[\\/]/).pop() ?? t("profiles.none");
  const encoding = profile ? `${profile.encoding.toUpperCase()} · ${profile.lineEnding.replace(/-/g, "").toUpperCase()}` : "";
  const filteredCount = editor.filteredSettings.length;

  return (
    <ConsoleFrame
      actions={<ProfileEditorToolbar className="console-cmd" editor={editor} variant="icons" />}
      bodyClassName="console-tuner-body"
      crumb={t("nav.tunerGroup")}
      status={<>
        <span className="app-statusbar-item"><StatusDot tone={dirtyCount > 0 ? "accent" : "neutral"} /> {t("profiles.unsavedSummary", { count: dirtyCount })}</span>
        <span className="app-statusbar-item"><code>{profileName}</code>{encoding && <> · {encoding}</>}</span>
        {editor.message && <span className="app-statusbar-item profile-message" aria-live="polite">{editor.message}</span>}
      </>}
      statusRight={<span className="app-statusbar-item">{mode === "quick" ? t("profiles.stepPosition", { current: editor.stepIndex + 1, total: editor.wizardStepIds.length }) : t("profiles.groupSummary", { group: editor.headingText, count: filteredCount })}</span>}
      summary={<span className="console-chip"><code>{profileName}</code>{dirtyCount > 0 && <span className="console-chip-dirty"> · {t("profiles.unsavedSummary", { count: dirtyCount })}</span>}</span>}
      title={title}
      titleId="profiles-title"
    >
      <ProfileSaveAsForm editor={editor} />
      <ConsolePanel className="console-index-panel" title={mode === "quick" ? t("profiles.stepsPanel") : t("profiles.sections")}>
        {mode === "advanced" && <label className="console-field console-search console-index-search"><Search aria-hidden="true" size={12} /><span className="sr-only">{t("profiles.search")}</span><input onChange={(event) => editor.setQuery(event.target.value)} placeholder={t("profiles.search")} type="search" value={editor.query} /></label>}
        <ul className="console-index">
          {mode === "quick"
            ? editor.wizardStepIds.map((step, index) => <li key={step}><button data-done={index < editor.stepIndex} data-selected={editor.activeWizardStep === step} onClick={() => editor.setActiveWizardStep(step)} type="button"><span className="console-index-num" aria-hidden="true">{String(index + 1).padStart(2, "0")}</span><span>{t(`wizard.${step}`)}</span></button></li>)
            : editor.groups.map((group) => <li key={group.id}><button data-selected={!editor.query && editor.activeGroup === group.id} onClick={() => editor.chooseGroup(group.id)} type="button"><span>{group.label}</span></button></li>)}
        </ul>
      </ConsolePanel>

      <div className="settings-workspace console-workspace" data-preview-docked={editor.previewDocked} ref={editor.workspaceRef}>
        <ConsolePanel className="console-settings-panel" scroll title={<span className="console-group-title"><strong>{mode === "quick" ? `${String(editor.stepIndex + 1).padStart(2, "0")} · ${editor.headingText}` : editor.headingText}</strong><span>{editor.headingHint}</span></span>}>
          {profile && !profile.canSave && <p className="console-note">{t("profiles.readOnly")}</p>}
          <div className="settings-form console-settings-form">
            <ProfileEditorBody editor={editor} />
          </div>
        </ConsolePanel>
        <ProfileEditorPreview ciSmoke={shell.ciSmoke} editor={editor} onOpenStudio={shell.openPreviewStudio} onPreviewReady={() => shell.reportReady("profiles")} />
      </div>
    </ConsoleFrame>
  );
}
