import { AlertTriangle, Check, Copy, Download, FileInput, FileText, FolderOpen, Play, Save } from "lucide-react";
import type { ShellProps } from "../../app/shell";
import { previewImageUrl } from "../../app/tauri";
import { matchesAppliedProfile, THUMBNAIL_SAMPLE_TEXT, useFileSettingsModel } from "../../features/files/useFileSettingsModel";
import { useI18n } from "../../i18n/i18n";
import { FluentCard, FluentCards, FluentPage, FluentSection } from "./FluentParts";

export function FluentFiles({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
  const model = useFileSettingsModel({ onEditInTuner: () => shell.navigate("profiles", "advanced") });
  const { profile, profiles, appliedProfile, legacy, thumbnails, busy, message, error } = model;

  return (
    <FluentPage
      actions={<button className="button secondary" disabled={busy !== null} onClick={() => void model.chooseImport()} type="button"><FileInput aria-hidden="true" size={16} strokeWidth={1.6} /> {busy === "import" ? t("files.importing") : t("files.chooseImport")}</button>}
      subtitle={t("files.subtitle")}
      title={t("nav.profiles")}
      titleId="files-title"
    >
      {legacy && (
        <FluentCards>
          <FluentCard
            action={<button className="button primary" disabled={busy !== null} onClick={() => void model.importFrom(legacy.path)} type="button">{busy === "import" ? t("files.importing") : t("files.importDetected")}</button>}
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
            const applied = matchesAppliedProfile(entry, appliedProfile);
            const thumbnail = thumbnails.get(entry.path) ?? null;
            return (
              <li className="fluent-pcard" data-applied={applied} data-selected={selected} key={entry.path}>
                <button aria-pressed={selected} className="fluent-pcard-select" disabled={busy !== null} onClick={() => void model.chooseProfile(entry.path)} type="button">
                  <span className="fluent-thumb">
                    {thumbnail ? <img alt={t("files.thumbnailAlt", { name: entry.name })} loading="lazy" src={previewImageUrl(thumbnail.imagePath)} /> : <span aria-hidden="true" className="fluent-thumb-fallback">{THUMBNAIL_SAMPLE_TEXT}</span>}
                  </span>
                  <span className="fluent-pcard-name"><strong>{entry.name}</strong>{applied && <span className="fluent-badge">{t("files.appliedBadge")}</span>}</span>
                  <code title={entry.path}>{entry.displayPath}</code>
                </button>
                <div className="fluent-pcard-foot"><button className="text-action fluent-link" disabled={busy !== null} onClick={() => void model.editInTuner(entry.path)} type="button">{t("files.editInTuner")}</button></div>
              </li>
            );
          })}
        </ul>
      </FluentSection>

      <FluentSection title={t("files.currentTitle")}>
        <FluentCards>
          <FluentCard
            action={<button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.revealCurrentProfile()} type="button"><FolderOpen aria-hidden="true" size={16} strokeWidth={1.6} /> {t("files.reveal")}</button>}
            description={profile ? `${model.encodingText} · ${t("files.unsaved")} ${model.unsavedText}${!profile.canSave ? ` · ${t("files.readOnly")}` : ""}` : undefined}
            icon={<FileText aria-hidden="true" size={20} strokeWidth={1.6} />}
            title={<>{profile ? t("files.editing") : t("profiles.none")}{profile && <> · <code title={profile.path}>{profile.displayPath}</code></>}</>}
          />
          <FluentCard
            action={<><input aria-label={t("profiles.copyName")} className="fluent-field" disabled={!profile || busy !== null} onChange={(event) => model.setCopyName(event.target.value)} placeholder={t("files.saveAsName")} value={model.copyName} /><button className="button secondary" disabled={!model.canDuplicate} onClick={() => void model.duplicate()} type="button">{t("profiles.save")}</button></>}
            description={t("files.duplicateDescription")}
            icon={<Copy aria-hidden="true" size={20} strokeWidth={1.6} />}
            title={t("files.saveAs")}
          />
          <FluentCard
            action={<button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.exportIni()} type="button">{busy === "export" ? t("files.exporting") : t("files.chooseExport")}</button>}
            description={t("files.exportDescription")}
            icon={<Download aria-hidden="true" size={20} strokeWidth={1.6} />}
            title={t("files.exportTitle")}
          />
        </FluentCards>
        <div className="fluent-footer-actions">
          <button className="button secondary" disabled={!model.canSave} onClick={() => void model.save()} type="button"><Save aria-hidden="true" size={16} strokeWidth={1.6} /> {busy === "save" ? t("profiles.saving") : t("profiles.save")}</button>
          <button className="button primary" disabled={!model.canApply} onClick={() => void model.apply()} title={model.dirtyCount > 0 ? t("profiles.saveBeforeApply") : undefined} type="button"><Play aria-hidden="true" size={16} strokeWidth={1.6} /> {busy === "apply" ? t("profiles.applying") : t("profiles.apply")}</button>
        </div>
      </FluentSection>

      {message && <p aria-live="polite" className="success-message" data-operation="file-settings"><Check aria-hidden="true" size={16} /> {message}</p>}
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
    </FluentPage>
  );
}
