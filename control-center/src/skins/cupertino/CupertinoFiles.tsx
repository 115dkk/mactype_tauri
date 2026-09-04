import { AlertTriangle, Check, FileInput, FolderOpen } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { previewImageUrl } from "../../app/tauri";
import { matchesAppliedProfile, THUMBNAIL_SAMPLE_TEXT, useFileSettingsModel } from "../../features/files/useFileSettingsModel";
import { useI18n } from "../../i18n/i18n";
import { CupertinoBadge, CupertinoGroup, CupertinoPage, CupertinoRow, CupertinoSection, CupertinoToolbar } from "./CupertinoParts";

export function CupertinoFiles({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
  const model = useFileSettingsModel({ onEditInTuner: () => shell.navigate("profiles", "advanced") });
  const { profile, profiles, appliedProfile, legacy, thumbnails, busy, message, error } = model;

  return (
    <CupertinoPage
      actions={<button className="button secondary" disabled={busy !== null} onClick={() => void model.chooseImport()} type="button">{busy === "import" ? t("files.importing") : `${t("files.chooseImport")}…`}</button>}
      subtitle={t("files.subtitle")}
      title={t("nav.profiles")}
      titleId="files-title"
    >
      {legacy && (
        <CupertinoGroup dataKind="legacy">
          <CupertinoRow
            description={<>{t("files.detectedDescription", { name: legacy.name })} <code title={legacy.path}>{legacy.path}</code></>}
            hero
            leading={<span className="cupertino-okc" data-tone="accent"><FileInput aria-hidden="true" size={15} strokeWidth={2.2} /></span>}
            title={t("files.detectedTitle")}
            value={<button className="button primary" disabled={busy !== null} onClick={() => void model.importFrom(legacy.path)} type="button">{busy === "import" ? t("files.importing") : t("files.importDetected")}</button>}
          />
        </CupertinoGroup>
      )}

      <CupertinoGroup className="cupertino-profile-list" dataKind="profiles">
        {profiles.map((entry) => {
          const selected = profile?.path === entry.path;
          const applied = matchesAppliedProfile(entry, appliedProfile);
          const thumbnail = thumbnails.get(entry.path) ?? null;
          return (
            <div className="cupertino-row cupertino-profile-row" data-applied={applied} data-leading="true" data-selected={selected} key={entry.path}>
              <label className="cupertino-radio-wrap">
                <input aria-label={entry.name} checked={selected} disabled={busy !== null} name="cupertino-profile" onChange={() => void model.chooseProfile(entry.path)} type="radio" value={entry.path} />
                <span aria-hidden="true" className="cupertino-radio" />
              </label>
              <span className="cupertino-thumb">
                {thumbnail ? <img alt={t("files.thumbnailAlt", { name: entry.name })} loading="lazy" src={previewImageUrl(thumbnail.imagePath)} /> : <span aria-hidden="true">{THUMBNAIL_SAMPLE_TEXT}</span>}
              </span>
              <div className="cupertino-row-copy">
                <div className="cupertino-row-title">{entry.name}{applied && <CupertinoBadge>{t("files.appliedBadge")}</CupertinoBadge>}</div>
                <div className="cupertino-row-desc"><code title={entry.path}>{entry.displayPath}</code></div>
              </div>
              <div className="cupertino-row-value"><button className="button secondary" disabled={busy !== null} onClick={() => void model.editInTuner(entry.path)} type="button">{t("files.editInTuner")}</button></div>
            </div>
          );
        })}
      </CupertinoGroup>

      <CupertinoSection title={t("files.currentTitle")}>
        <CupertinoGroup dataKind="current">
          <CupertinoRow title={t("files.editing")} value={<><code title={profile?.path}>{profile?.displayPath ?? t("profiles.none")}</code><button aria-label={t("files.reveal")} className="button icon" disabled={!profile || busy !== null} onClick={() => void model.revealCurrentProfile()} title={t("files.reveal")} type="button"><FolderOpen aria-hidden="true" size={13} strokeWidth={1.8} /></button></>} />
          <CupertinoRow title={`${t("files.encoding")} · ${t("files.lineEnding")}`} value={model.encodingText} />
          <CupertinoRow title={t("files.unsaved")} value={model.unsavedText} />
          <CupertinoRow description={t("files.duplicateDescription")} title={t("files.saveAs")} value={<><input aria-label={t("profiles.copyName")} className="cupertino-field" disabled={!profile || busy !== null} onChange={(event) => model.setCopyName(event.target.value)} placeholder={t("files.saveAsName")} value={model.copyName} /><button className="button secondary" disabled={!model.canDuplicate} onClick={() => void model.duplicate()} type="button">{t("profiles.save")}</button></>} />
        </CupertinoGroup>
        {profile && !profile.canSave && <p className="cupertino-footnote">{t("files.readOnly")}</p>}
      </CupertinoSection>

      <CupertinoToolbar>
        <button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.exportIni()} type="button">{busy === "export" ? t("files.exporting") : `${t("files.chooseExport")}…`}</button>
        <span className="cupertino-spacer" />
        <button className="button secondary" disabled={!model.canSave} onClick={() => void model.save()} type="button">{busy === "save" ? t("profiles.saving") : t("profiles.save")}</button>
        <button className="button primary" disabled={!model.canApply} onClick={() => void model.apply()} title={model.dirtyCount > 0 ? t("profiles.saveBeforeApply") : undefined} type="button">{busy === "apply" ? t("profiles.applying") : t("profiles.apply")}</button>
      </CupertinoToolbar>

      {message && <p aria-live="polite" className="success-message" data-operation="file-settings"><Check aria-hidden="true" size={16} /> {message}</p>}
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
    </CupertinoPage>
  );
}
