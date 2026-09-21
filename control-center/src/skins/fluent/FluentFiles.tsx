import { Copy, Download, FileInput, FileText, FolderOpen, Save } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { runtime } from "../../app/runtimeAdapter";
import { CurrentFileSummary, FileNameDialog, DesignateAction, FileMessages, RunProfileBadge } from "../../features/files/FileParts";
import { THUMBNAIL_SAMPLE_TEXT, useFileSettingsModel } from "../../features/files/useFileSettingsModel";
import { useI18n } from "../../i18n/i18n";
import { FluentCard, FluentCards, FluentPage, FluentSection } from "./FluentParts";

export function FluentFiles({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
  const model = useFileSettingsModel({ onEditInTuner: shell.operations.editInTuner });
  const { profile } = model.document;
  const { profiles, thumbnails } = model.profiles;
  const { legacy } = model.legacy;
  const { busy } = model.files;

  return (
    <FluentPage
      actions={<button className="button secondary" disabled={busy !== null} onClick={() => void model.files.chooseImport()} type="button"><FileInput aria-hidden="true" size={16} strokeWidth={1.6} /> {busy === "import" ? t("files.importing") : t("files.chooseImport")}</button>}
      subtitle={t("files.subtitle")}
      title={t("nav.profiles")}
      titleId="files-title"
    >
      {legacy && (
        <FluentCards>
          <FluentCard
            action={<button className="button primary" disabled={busy !== null} onClick={() => void model.legacy.importFrom(legacy.path)} type="button">{busy === "import" ? t("files.importing") : t("files.importDetected")}</button>}
            description={<>{t("files.detectedDescription", { name: legacy.name })} <code title={legacy.path}>{legacy.path}</code></>}
            icon={<FileInput aria-hidden="true" size={20} strokeWidth={1.6} />}
            title={t("files.detectedTitle")}
          />
        </FluentCards>
      )}

      <FluentSection hint={t("files.selectDescription")} title={t("profiles.select")}>
        <ul className="fluent-gallery">
          {profiles.map((entry) => {
            const selected = profile?.path === entry.path;
            const thumbnail = thumbnails.get(entry.path) ?? null;
            return (
              <li className="fluent-pcard" {...model.profiles.runProfileAttributes(entry)} data-selected={selected} key={entry.path}>
                <button aria-pressed={selected} className="fluent-pcard-select" disabled={busy !== null} onClick={() => void model.profiles.chooseProfile(entry.path)} type="button">
                  <span className="fluent-thumb">
                    {thumbnail ? <img alt={t("files.thumbnailAlt", { name: entry.name })} loading="lazy" src={runtime().previewImageUrl(thumbnail.imagePath)} /> : <span aria-hidden="true" className="fluent-thumb-fallback">{THUMBNAIL_SAMPLE_TEXT}</span>}
                  </span>
                  <span className="fluent-pcard-name"><strong>{entry.name}</strong><RunProfileBadge className="fluent-badge" entry={entry} model={model} /></span>
                  <code title={entry.path}>{entry.displayPath}</code>
                </button>
                <div className="fluent-pcard-foot"><button className="text-action fluent-link" disabled={busy !== null} onClick={() => void model.profiles.editInTuner(entry.path)} type="button">{t("files.editInTuner")}</button></div>
              </li>
            );
          })}
        </ul>
      </FluentSection>

      <FluentSection title={t("files.currentTitle")}>
        <FluentCards>
          <FluentCard
            action={<button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.files.revealCurrentProfile()} type="button"><FolderOpen aria-hidden="true" size={16} strokeWidth={1.6} /> {t("files.reveal")}</button>}
            description={profile ? <CurrentFileSummary model={model} variant="description" /> : undefined}
            icon={<FileText aria-hidden="true" size={20} strokeWidth={1.6} />}
            title={<CurrentFileSummary model={model} variant="title" />}
          />
          <FluentCard
            action={<><button className="button secondary" disabled={!model.files.canDuplicate} onClick={() => model.files.setNameDialogOpen(true)} type="button">{t("files.saveAs")}</button></>}
            description={t("files.duplicateDescription")}
            icon={<Copy aria-hidden="true" size={20} strokeWidth={1.6} />}
            title={t("files.saveAs")}
          />
          <FluentCard
            action={<button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.files.exportIni()} type="button">{busy === "export" ? t("files.exporting") : t("files.chooseExport")}</button>}
            description={t("files.exportDescription")}
            icon={<Download aria-hidden="true" size={20} strokeWidth={1.6} />}
            title={t("files.exportTitle")}
          />
        </FluentCards>
        <div className="fluent-footer-actions">
          <button className="button secondary" disabled={!model.document.canSave} onClick={() => void model.document.save()} type="button"><Save aria-hidden="true" size={16} strokeWidth={1.6} /> {busy === "save" ? t("profiles.saving") : t("profiles.save")}</button>
          <DesignateAction model={model} variant="fluent" />
        </div>
      </FluentSection>

      <FileNameDialog model={model} />
      <FileMessages model={model} variant="fluent" />
    </FluentPage>
  );
}
