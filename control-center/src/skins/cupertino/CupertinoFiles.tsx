import { FileInput, FolderOpen } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { runtime } from "../../app/runtimeAdapter";
import { CurrentFileSummary, DesignateAction, FileMessages, RunProfileBadge } from "../../features/files/FileParts";
import { THUMBNAIL_SAMPLE_TEXT, useFileSettingsModel } from "../../features/files/useFileSettingsModel";
import { useI18n } from "../../i18n/i18n";
import { CupertinoGroup, CupertinoPage, CupertinoRow, CupertinoSection, CupertinoToolbar } from "./CupertinoParts";

export function CupertinoFiles({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
  const model = useFileSettingsModel({ onEditInTuner: shell.operations.editInTuner });
  const { profile } = model.document;
  const { profiles, thumbnails } = model.profiles;
  const { legacy } = model.legacy;
  const { busy } = model.files;

  return (
    <CupertinoPage
      actions={<button className="button secondary" disabled={busy !== null} onClick={() => void model.files.chooseImport()} type="button">{busy === "import" ? t("files.importing") : `${t("files.chooseImport")}…`}</button>}
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
            value={<button className="button primary" disabled={busy !== null} onClick={() => void model.legacy.importFrom(legacy.path)} type="button">{busy === "import" ? t("files.importing") : t("files.importDetected")}</button>}
          />
        </CupertinoGroup>
      )}

      <CupertinoGroup className="cupertino-profile-list" dataKind="profiles">
        {profiles.map((entry) => {
          const selected = profile?.path === entry.path;
          const thumbnail = thumbnails.get(entry.path) ?? null;
          return (
            <div className="cupertino-row cupertino-profile-row" {...model.profiles.runProfileAttributes(entry)} data-leading="true" data-selected={selected} key={entry.path}>
              <label className="cupertino-radio-wrap">
                <input aria-label={entry.name} checked={selected} disabled={busy !== null} name="cupertino-profile" onChange={() => void model.profiles.chooseProfile(entry.path)} type="radio" value={entry.path} />
                <span aria-hidden="true" className="cupertino-radio" />
              </label>
              <span className="cupertino-thumb">
                {thumbnail ? <img alt={t("files.thumbnailAlt", { name: entry.name })} loading="lazy" src={runtime().previewImageUrl(thumbnail.imagePath)} /> : <span aria-hidden="true">{THUMBNAIL_SAMPLE_TEXT}</span>}
              </span>
              <div className="cupertino-row-copy">
                <div className="cupertino-row-title">{entry.name}<RunProfileBadge className="cupertino-badge" entry={entry} model={model} /></div>
                <div className="cupertino-row-desc"><code title={entry.path}>{entry.displayPath}</code></div>
              </div>
              <div className="cupertino-row-value"><button className="button secondary" disabled={busy !== null} onClick={() => void model.profiles.editInTuner(entry.path)} type="button">{t("files.editInTuner")}</button></div>
            </div>
          );
        })}
      </CupertinoGroup>

      <CupertinoSection title={t("files.currentTitle")}>
        <CupertinoGroup dataKind="current">
          <CupertinoRow title={t("files.editing")} value={<><CurrentFileSummary model={model} /><button aria-label={t("files.reveal")} className="button icon" disabled={!profile || busy !== null} onClick={() => void model.files.revealCurrentProfile()} title={t("files.reveal")} type="button"><FolderOpen aria-hidden="true" size={13} strokeWidth={1.8} /></button></>} />
          <CupertinoRow title={`${t("files.encoding")} · ${t("files.lineEnding")}`} value={<CurrentFileSummary model={model} variant="encoding" />} />
          <CupertinoRow title={t("files.unsaved")} value={<CurrentFileSummary model={model} variant="unsaved" />} />
          <CupertinoRow description={t("files.duplicateDescription")} title={t("files.saveAs")} value={<><input aria-label={t("profiles.copyName")} className="cupertino-field" disabled={!profile || busy !== null} onChange={(event) => model.files.setCopyName(event.target.value)} placeholder={t("files.saveAsName")} value={model.files.copyName} /><button className="button secondary" disabled={!model.files.canDuplicate} onClick={() => void model.files.duplicate()} type="button">{t("profiles.save")}</button></>} />
        </CupertinoGroup>
        {profile && !profile.canSave && <p className="cupertino-footnote">{t("files.readOnly")}</p>}
      </CupertinoSection>

      <CupertinoToolbar>
        <button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.files.exportIni()} type="button">{busy === "export" ? t("files.exporting") : `${t("files.chooseExport")}…`}</button>
        <span className="cupertino-spacer" />
        <button className="button secondary" disabled={!model.document.canSave} onClick={() => void model.document.save()} type="button">{busy === "save" ? t("profiles.saving") : t("profiles.save")}</button>
        <DesignateAction model={model} variant="cupertino" />
      </CupertinoToolbar>

      <FileMessages model={model} variant="cupertino" />
    </CupertinoPage>
  );
}
