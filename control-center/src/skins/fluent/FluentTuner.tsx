import { Check, Search } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { ProfileEditorBody, ProfileEditorPreview, ProfileEditorSummary, ProfileEditorToolbar, ProfileSaveAsForm } from "../../features/profiles/ProfileEditorParts";
import { useProfileEditor } from "../../features/profiles/useProfileEditor";
import { useI18n } from "../../i18n/i18n";
import { FluentPage } from "./FluentParts";

export function FluentTuner({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
  const mode = shell.navigation.profileMode;
  const editor = useProfileEditor({ mode });

  return (
    <FluentPage
      actions={<ProfileEditorToolbar className="fluent-cmd" editor={editor} variant="icons" />}
      crumb={t("nav.tunerGroup")}
      subtitle={<ProfileEditorSummary editor={editor} />}
      title={t(mode === "quick" ? "nav.guidedSetup" : "nav.allSettings")}
      titleId="profiles-title"
      wide
    >
      <ProfileSaveAsForm editor={editor} />
      <div className="fluent-two profile-layout" data-mode={mode}>
        <aside className="fluent-index settings-index" aria-label={mode === "quick" ? t("wizard.progress") : t("profiles.sections")}>
          {mode === "advanced" && <label className="fluent-field fluent-search search-field"><Search aria-hidden="true" size={14} strokeWidth={1.8} /><span className="sr-only">{t("profiles.search")}</span><input onChange={(event) => editor.editing.setQuery(event.target.value)} placeholder={t("profiles.search")} type="search" value={editor.editing.query} /></label>}
          <ul className="fluent-index-list">
            {mode === "quick"
              ? editor.editing.wizardStepIds.map((step, index) => {
                const done = index < editor.editing.stepIndex;
                return <li key={step}><button data-selected={editor.editing.activeWizardStep === step} onClick={() => editor.editing.setActiveWizardStep(step)} type="button"><span aria-hidden="true" className="fluent-step-num" data-done={done}>{done ? <Check aria-hidden="true" size={12} strokeWidth={2.4} /> : index + 1}</span><span>{t(`wizard.${step}`)}</span></button></li>;
              })
              : editor.editing.groups.map((group) => <li key={group.id}><button data-selected={!editor.editing.query && editor.editing.activeGroup === group.id} onClick={() => editor.editing.chooseGroup(group.id)} type="button"><span>{group.label}</span></button></li>)}
          </ul>
        </aside>
        <div className="settings-workspace fluent-workspace" data-preview-docked={editor.preview.previewDocked} ref={editor.preview.workspaceRef}>
          <div className="settings-form fluent-main">
            <h2 className="fluent-sub-title">{editor.editing.headingText}</h2>
            <p className="fluent-desc">{editor.editing.headingHint}</p>
            <ProfileEditorBody editor={editor} />
          </div>
          <ProfileEditorPreview ciSmoke={shell.operations.ciSmoke} editor={editor} onOpenStudio={shell.operations.openPreviewStudio} onPreviewReady={() => shell.operations.reportReady("profiles")} />
        </div>
      </div>
    </FluentPage>
  );
}
