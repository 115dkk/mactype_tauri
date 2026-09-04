import { Search } from "lucide-react";
import { Hint } from "../components/Hint";
import { ProfileEditorBody, ProfileEditorHeading, ProfileEditorPreview, ProfileEditorSummary, ProfileEditorToolbar, ProfileSaveAsForm } from "../features/profiles/ProfileEditorParts";
import { useProfileEditor, type ProfileMode } from "../features/profiles/useProfileEditor";

interface ProfilesPageProps {
  ciSmoke?: boolean;
  mode?: ProfileMode;
  onModeChange?: (mode: ProfileMode) => void;
  onPreviewReady?: () => void;
  onOpenStudio?: () => void;
}

export function ProfilesPage({ ciSmoke = false, mode = "advanced", onPreviewReady, onOpenStudio }: ProfilesPageProps) {
  const editor = useProfileEditor({ mode });
  const { t } = editor;
  return (
    <section className="page profile-page view-enter" aria-labelledby="profiles-title" data-mode={mode}>
      <header className="page-header compact profile-header">
        <div>
          <div className="profile-mode-title"><h1 id="profiles-title"><Hint content={t(mode === "quick" ? "profiles.quickDescription" : "profiles.advancedDescription")}>{t(mode === "quick" ? "nav.guidedSetup" : "nav.allSettings")}</Hint></h1><span>Tuner</span></div>
          <ProfileEditorSummary editor={editor} />
        </div>
        <ProfileEditorToolbar editor={editor} />
      </header>

      <ProfileSaveAsForm editor={editor} />

      <div className="profile-layout">
        <aside className="settings-index" aria-label={mode === "quick" ? t("wizard.progress") : t("profiles.sections")}>
          {mode === "advanced" && <label className="search-field"><Search aria-hidden="true" size={16} /><span className="sr-only">{t("profiles.search")}</span><input onChange={(event) => editor.setQuery(event.target.value)} placeholder={t("profiles.search")} type="search" value={editor.query} /></label>}
          <ul>{mode === "quick" ? editor.wizardStepIds.map((step, index) => <li key={step}><button data-selected={editor.activeWizardStep === step} onClick={() => editor.setActiveWizardStep(step)} type="button"><span className="settings-step" aria-hidden="true">{index + 1}</span><span>{t(`wizard.${step}`)}</span></button></li>) : editor.groups.map((group) => <li key={group.id}><button data-selected={!editor.query && editor.activeGroup === group.id} onClick={() => editor.chooseGroup(group.id)} type="button"><span>{group.label}</span></button></li>)}</ul>
        </aside>

        <div className="settings-workspace" data-preview-docked={editor.previewDocked} ref={editor.workspaceRef}>
          <div className="settings-form">
            <div className="section-heading"><ProfileEditorHeading editor={editor} /></div>
            <ProfileEditorBody editor={editor} />
          </div>
          <ProfileEditorPreview ciSmoke={ciSmoke} editor={editor} onOpenStudio={onOpenStudio} onPreviewReady={onPreviewReady} />
        </div>
      </div>
    </section>
  );
}
