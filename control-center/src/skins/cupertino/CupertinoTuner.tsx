import { Check, Search } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { ProfileEditorBody, ProfileEditorPreview, ProfileEditorSummary, ProfileEditorToolbar, ProfileSaveAsForm } from "../../features/profiles/ProfileEditorParts";
import { useProfileEditor } from "../../features/profiles/useProfileEditor";
import { useI18n } from "../../i18n/i18n";
import { CupertinoPage } from "./CupertinoParts";

export function CupertinoTuner({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
  const mode = shell.navigation.profileMode;
  const editor = useProfileEditor({ mode });

  return (
    <CupertinoPage
      actions={<ProfileEditorToolbar className="cupertino-cmd" editor={editor} variant="icons" />}
      subtitle={<ProfileEditorSummary editor={editor} />}
      title={t(mode === "guided" ? "nav.guidedSetup" : "nav.allSettings")}
      titleId="profiles-title"
      wide
    >
      <ProfileSaveAsForm editor={editor} />
      <div className="cupertino-three profile-layout" data-mode={mode}>
        <aside className="cupertino-group cupertino-list settings-index" aria-label={mode === "guided" ? t("guided.progress") : t("profiles.sections")}>
          {mode === "all" && <div className="cupertino-row cupertino-list-search"><label className="cupertino-search"><Search aria-hidden="true" size={13} strokeWidth={2} /><span className="sr-only">{t("profiles.search")}</span><input onChange={(event) => editor.editing.setQuery(event.target.value)} placeholder={t("profiles.search")} type="search" value={editor.editing.query} /></label></div>}
          <ul className="cupertino-list-items">
            {mode === "guided"
              ? editor.editing.guidedStepIds.map((step, index) => {
                const done = index < editor.editing.stepIndex;
                return <li key={step}><button className="cupertino-row cupertino-list-row" data-selected={editor.editing.activeGuidedStep === step} onClick={() => editor.editing.setActiveGuidedStep(step)} type="button"><span aria-hidden="true" className="cupertino-n" data-done={done}>{done ? <Check aria-hidden="true" size={10} strokeWidth={3} /> : index + 1}</span><span>{t(`guided.${step}`)}</span></button></li>;
              })
              : editor.editing.groups.map((group) => <li key={group.id}><button className="cupertino-row cupertino-list-row" data-selected={!editor.editing.query && editor.editing.activeGroup === group.id} onClick={() => editor.editing.chooseGroup(group.id)} type="button"><span>{group.label}</span></button></li>)}
          </ul>
        </aside>
        <div className="settings-workspace cupertino-workspace" data-preview-docked={editor.preview.previewDocked} ref={editor.preview.workspaceRef}>
          <div className="settings-form cupertino-mid">
            <h2>{editor.editing.headingText}</h2>
            <p className="cupertino-desc">{editor.editing.headingHint}</p>
            <div className="cupertino-group cupertino-settings-group">
              <ProfileEditorBody editor={editor} />
            </div>
          </div>
          <ProfileEditorPreview ciSmoke={shell.operations.ciSmoke} editor={editor} onOpenStudio={shell.operations.openPreviewStudio} onPreviewReady={() => shell.operations.reportReady("profiles")} />
        </div>
      </div>
    </CupertinoPage>
  );
}
