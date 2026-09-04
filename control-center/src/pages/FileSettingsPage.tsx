import { AlertTriangle, Check, FileInput, FileOutput, FolderOpen, Play, Save, SaveAll, SlidersHorizontal } from "lucide-react";
import { previewImageUrl } from "../app/tauri";
import { matchesAppliedProfile, THUMBNAIL_SAMPLE_TEXT, useFileSettingsModel } from "../features/files/useFileSettingsModel";

interface FileSettingsPageProps {
  onEditInTuner?: () => void;
}

export function FileSettingsPage({ onEditInTuner }: FileSettingsPageProps) {
  const model = useFileSettingsModel({ onEditInTuner });
  const { t, profile, profiles, appliedProfile, legacy, thumbnails, copyName, busy, message, error, dirtyCount } = model;

  return (
    <section className="page view-enter" aria-labelledby="files-title">
      <header className="page-header">
        <div><h1 id="files-title">{t("nav.profiles")}</h1><p>{t("files.subtitle")}</p></div>
        <div className="header-actions">
          <button className="button" disabled={busy !== null} onClick={() => void model.chooseImport()} type="button"><FileInput aria-hidden="true" size={17} /> {busy === "import" ? t("files.importing") : t("files.chooseImport")}</button>
        </div>
      </header>

      {legacy && (
        <section className="legacy-import-banner" aria-labelledby="legacy-import-title">
          <FileInput aria-hidden="true" size={22} />
          <div>
            <h2 id="legacy-import-title">{t("files.detectedTitle")}</h2>
            <p>{t("files.detectedDescription", { name: legacy.name })}</p>
            <code title={legacy.path}>{legacy.path}</code>
          </div>
          <button className="button primary" disabled={busy !== null} onClick={() => void model.importFrom(legacy.path)} type="button">
            {busy === "import" ? t("files.importing") : t("files.importDetected")}
          </button>
        </section>
      )}

      <section className="section-block" aria-labelledby="profile-select-title">
        <div className="section-heading"><div><h2 id="profile-select-title">{t("profiles.select")}</h2><p>{t("files.selectDescription")}</p></div></div>
        <ul className="profile-grid">
          {profiles.map((entry) => {
            const selected = profile?.path === entry.path;
            const applied = matchesAppliedProfile(entry, appliedProfile);
            const thumbnail = thumbnails.get(entry.path) ?? null;
            return (
              <li className="profile-card" data-applied={applied} data-selected={selected} key={entry.path}>
                <button aria-pressed={selected} className="profile-card-select" disabled={busy !== null} onClick={() => void model.chooseProfile(entry.path)} type="button">
                  <span className="profile-card-thumb">
                    {thumbnail
                      ? <img alt={t("files.thumbnailAlt", { name: entry.name })} loading="lazy" src={previewImageUrl(thumbnail.imagePath)} />
                      : <span aria-hidden="true" className="profile-card-thumb-fallback">{THUMBNAIL_SAMPLE_TEXT}</span>}
                  </span>
                  <span className="profile-card-title">
                    <strong>{entry.name}</strong>
                    {applied && <span className="profile-card-badge">{t("files.appliedBadge")}</span>}
                  </span>
                  <code title={entry.path}>{entry.displayPath}</code>
                </button>
                <div className="profile-card-actions">
                  <button className="text-action" disabled={busy !== null} onClick={() => void model.editInTuner(entry.path)} type="button"><SlidersHorizontal aria-hidden="true" size={14} /> {t("files.editInTuner")}</button>
                </div>
              </li>
            );
          })}
        </ul>
      </section>

      <section className="section-block" aria-labelledby="current-file-title">
        <div className="section-heading"><div><h2 id="current-file-title">{t("files.currentTitle")}</h2><p>{t("files.currentDescription")}</p></div></div>
        <div className="selected-file-area">
          <div className="selected-file-summary" data-empty={!profile}>
            <FolderOpen aria-hidden="true" size={22} />
            <div><strong>{profile ? t("files.editing") : t("profiles.none")}</strong>{profile && <div className="selected-file-path"><code title={profile.path}>{profile.displayPath}</code><button aria-label={t("files.reveal")} className="icon-button" disabled={busy !== null} onClick={() => void model.revealCurrentProfile()} title={t("files.reveal")} type="button"><FolderOpen aria-hidden="true" size={15} /></button></div>}</div>
          </div>
        </div>
        {profile && !profile.canSave && <p className="file-save-warning">{t("files.readOnly")}</p>}
        <details className="file-details">
          <summary>{model.detailsSummary}</summary>
          <dl className="detail-list compact-details">
            <div><dt>{t("files.encoding")}</dt><dd>{profile?.encoding ?? "—"}</dd></div>
            <div><dt>{t("files.lineEnding")}</dt><dd>{profile?.lineEnding ?? "—"}</dd></div>
            <div><dt>{t("files.unsaved")}</dt><dd>{dirtyCount ? t("files.unsavedCount", { count: dirtyCount }) : t("files.noUnsaved")}</dd></div>
          </dl>
        </details>
        <div className="file-primary-actions">
          <button className="button secondary" disabled={!model.canSave} onClick={() => void model.save()} type="button"><Save aria-hidden="true" size={17} /> {busy === "save" ? t("profiles.saving") : t("profiles.save")}</button>
          <div className="file-save-as"><input aria-label={t("profiles.copyName")} disabled={!profile || busy !== null} onChange={(event) => model.setCopyName(event.target.value)} placeholder={t("files.saveAsName")} value={copyName} /><button className="button secondary" disabled={!model.canDuplicate} onClick={() => void model.duplicate()} type="button"><SaveAll aria-hidden="true" size={16} /> {t("files.saveAs")}</button></div>
          <button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.exportIni()} type="button"><FileOutput aria-hidden="true" size={17} /> {busy === "export" ? t("files.exporting") : t("files.chooseExport")}</button>
          <button className="button primary" disabled={!model.canApply} onClick={() => void model.apply()} title={dirtyCount > 0 ? t("profiles.saveBeforeApply") : undefined} type="button"><Play aria-hidden="true" size={17} /> {busy === "apply" ? t("profiles.applying") : t("profiles.apply")}</button>
        </div>
      </section>

      {message && <p aria-live="polite" className="success-message" data-operation="file-settings"><Check aria-hidden="true" size={16} /> {message}</p>}
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
    </section>
  );
}
