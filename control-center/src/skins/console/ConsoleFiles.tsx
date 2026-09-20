import { FileInput, FileOutput, FolderOpen, Save, SaveAll, Search, SlidersHorizontal } from "lucide-react";
import { useState } from "react";
import { CurrentFileSummary, DesignateAction, FileMessages, RunProfileBadge } from "../../features/files/FileParts";
import { useFileSettingsModel } from "../../features/files/useFileSettingsModel";
import { SpecimenBoard } from "../../features/preview/SpecimenBoard";
import { substitutedPreviewFont } from "../../features/preview/previewFonts";
import { scriptUiFont } from "../../features/preview/scriptUiFont";
import { useI18n } from "../../i18n/i18n";
import { ConsoleFrame, ConsoleKv, ConsolePanel } from "./ConsoleFrame";
import { useConsole } from "./consoleContext";
import { ConsoleServiceStatus } from "./ConsoleStatus";

const SPECIMEN_SIZES = [18, 14, 11] as const;

export function ConsoleFiles() {
  const { locale, t } = useI18n();
  const { shell } = useConsole();
  const model = useFileSettingsModel({ onEditInTuner: shell.operations.editInTuner });
  const { profile } = model.document;
  const { profiles } = model.profiles;
  const { legacy } = model.legacy;
  const { busy } = model.files;
  const { message, error } = model.messages;
  const [filter, setFilter] = useState("");
  const needle = filter.trim().toLocaleLowerCase();
  const visible = profiles.filter((entry) => !needle || entry.name.toLocaleLowerCase().includes(needle) || entry.displayPath.toLocaleLowerCase().includes(needle));
  const fontFace = substitutedPreviewFont(scriptUiFont(locale) ?? "Segoe UI", profile?.values.font_substitutes === 0 ? [] : profile?.advanced.fontSubstitutes ?? []);

  return (
    <ConsoleFrame
      actions={<>
        <button className="button secondary" disabled={busy !== null} onClick={() => void model.files.chooseImport()} type="button"><FileInput aria-hidden="true" size={14} /> {busy === "import" ? t("files.importing") : t("files.chooseImport")}</button>
        <button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.files.exportIni()} type="button"><FileOutput aria-hidden="true" size={14} /> {busy === "export" ? t("files.exporting") : t("files.chooseExport")}</button>
      </>}
      bodyClassName="console-cols-main-side-wide"
      crumb={t("nav.wizardGroup")}
      status={<ConsoleServiceStatus />}
      statusRight={<span className="app-statusbar-item">{model.document.encodingText}</span>}
      summary={t("files.count", { count: profiles.length })}
      title={t("nav.profiles")}
      titleId="files-title"
    >
      <ConsolePanel
        footer={<>
          <FileMessages model={model} variant="console" />
          {!message && !error && <span className="console-muted">{profile && !profile.canSave ? t("files.readOnly") : t("files.selectDescription")}</span>}
        </>}
        right={<label className="console-field console-search"><Search aria-hidden="true" size={12} /><span className="sr-only">{t("files.search")}</span><input onChange={(event) => setFilter(event.target.value)} placeholder={t("files.search")} type="search" value={filter} /></label>}
        title={t("profiles.select")}
      >
        {legacy && (
          <div className="console-note console-note-action" data-legacy-import>
            <FileInput aria-hidden="true" size={14} />
            <span><strong>{t("files.detectedTitle")}</strong> {t("files.detectedDescription", { name: legacy.name })}</span>
            <button className="button primary" disabled={busy !== null} onClick={() => void model.legacy.importFrom(legacy.path)} type="button">{busy === "import" ? t("files.importing") : t("files.importDetected")}</button>
          </div>
        )}
        <div className="console-table" role="table" aria-label={t("profiles.select")}>
          <div className="console-table-head" role="row"><span role="columnheader">{t("files.columnName")}</span><span role="columnheader">{t("files.columnFile")}</span><span role="columnheader">{t("files.columnState")}</span></div>
          {visible.map((entry) => {
            const selected = profile?.path === entry.path;
            return (
              <button aria-pressed={selected} className="console-table-row" {...model.profiles.runProfileAttributes(entry)} data-selected={selected} disabled={busy !== null} key={entry.path} onClick={() => void model.profiles.chooseProfile(entry.path)} onDoubleClick={() => void model.profiles.editInTuner(entry.path)} role="row" type="button">
                <strong role="cell">{entry.name}</strong>
                <code role="cell" title={entry.path}>{entry.displayPath}</code>
                <span role="cell"><RunProfileBadge className="console-tag ok" entry={entry} model={model} /></span>
              </button>
            );
          })}
        </div>
      </ConsolePanel>

      <ConsolePanel
        footer={<>
          <button className="button secondary" disabled={!profile || busy !== null} onClick={() => profile && void model.profiles.editInTuner(profile.path)} type="button"><SlidersHorizontal aria-hidden="true" size={14} /> {t("files.editInTuner")}</button>
          <button className="button ghost" disabled={!profile || busy !== null} onClick={() => void model.files.revealCurrentProfile()} type="button"><FolderOpen aria-hidden="true" size={14} /> {t("files.reveal")}</button>
          <span className="console-spacer" />
          <button className="button secondary" disabled={!model.document.canSave} onClick={() => void model.document.save()} type="button"><Save aria-hidden="true" size={14} /> {busy === "save" ? t("profiles.saving") : t("profiles.save")}</button>
          <DesignateAction model={model} variant="console" />
        </>}
        right={<RunProfileBadge className="console-tag ok" entry={profile && { name: "", path: profile.path, displayPath: profile.displayPath }} model={model} />}
        scroll={false}
        title={t("files.selectedTitle")}
      >
        <SpecimenBoard className="specimen-board console-canvas console-canvas-fixed" dark={shell.preferences.theme === "dark"} fontFace={fontFace} profilePath={profile?.path ?? null} sizes={SPECIMEN_SIZES} text={t("profiles.samplePangram")} />
        <ConsoleKv rows={[
          { key: "file", label: t("files.columnFile"), value: <CurrentFileSummary model={model} /> },
          { key: "unsaved", label: t("files.unsaved"), value: <CurrentFileSummary model={model} variant="unsaved" /> },
          { key: "encoding", label: t("files.encoding"), value: <CurrentFileSummary model={model} variant="encoding" /> },
        ]} />
        <div className="console-spacer" />
        <div className="console-saveas">
          <input aria-label={t("profiles.copyName")} className="console-field" disabled={!profile || busy !== null} onChange={(event) => model.files.setCopyName(event.target.value)} placeholder={t("files.saveAsName")} value={model.files.copyName} />
          <button className="button secondary" disabled={!model.files.canDuplicate} onClick={() => void model.files.duplicate()} type="button"><SaveAll aria-hidden="true" size={14} /> {t("files.saveAs")}</button>
        </div>
      </ConsolePanel>
    </ConsoleFrame>
  );
}
