import { AlertTriangle, Check, FileInput, FileOutput, FolderOpen, Play, Save, SaveAll, Search, SlidersHorizontal } from "lucide-react";
import { useState } from "react";
import { matchesAppliedProfile, useFileSettingsModel } from "../../features/files/useFileSettingsModel";
import { SpecimenBoard } from "../../features/preview/SpecimenBoard";
import { useI18n } from "../../i18n/i18n";
import { ConsoleFrame, ConsoleKv, ConsolePanel } from "./ConsoleFrame";
import { useConsole } from "./consoleContext";
import { ConsoleServiceStatus } from "./ConsoleStatus";

const SPECIMEN_SIZES = [18, 14, 11] as const;

export function ConsoleFiles() {
  const { t } = useI18n();
  const { shell } = useConsole();
  const model = useFileSettingsModel({ onEditInTuner: () => shell.navigate("profiles", "advanced") });
  const { profile, profiles, appliedProfile, legacy, busy, message, error } = model;
  const [filter, setFilter] = useState("");
  const needle = filter.trim().toLocaleLowerCase();
  const visible = profiles.filter((entry) => !needle || entry.name.toLocaleLowerCase().includes(needle) || entry.displayPath.toLocaleLowerCase().includes(needle));
  const applied = profile ? matchesAppliedProfile({ name: "", path: profile.path, displayPath: profile.displayPath }, appliedProfile) : false;

  return (
    <ConsoleFrame
      actions={<>
        <button className="button secondary" disabled={busy !== null} onClick={() => void model.chooseImport()} type="button"><FileInput aria-hidden="true" size={14} /> {busy === "import" ? t("files.importing") : t("files.chooseImport")}</button>
        <button className="button secondary" disabled={!profile || busy !== null} onClick={() => void model.exportIni()} type="button"><FileOutput aria-hidden="true" size={14} /> {busy === "export" ? t("files.exporting") : t("files.chooseExport")}</button>
      </>}
      bodyClassName="console-cols-main-side-wide"
      crumb={t("nav.wizardGroup")}
      status={<ConsoleServiceStatus />}
      statusRight={<span className="app-statusbar-item">{model.encodingText}</span>}
      summary={t("files.count", { count: profiles.length })}
      title={t("nav.profiles")}
      titleId="files-title"
    >
      <ConsolePanel
        footer={<>
          {message && <span className="success-message" data-operation="file-settings"><Check aria-hidden="true" size={14} /> {message}</span>}
          {error && <span className="inline-error"><AlertTriangle aria-hidden="true" size={14} /> {error}</span>}
          {!message && !error && <span className="console-muted">{profile && !profile.canSave ? t("files.readOnly") : t("files.selectDescription")}</span>}
        </>}
        right={<label className="console-field console-search"><Search aria-hidden="true" size={12} /><span className="sr-only">{t("files.search")}</span><input onChange={(event) => setFilter(event.target.value)} placeholder={t("files.search")} type="search" value={filter} /></label>}
        title={t("profiles.select")}
      >
        {legacy && (
          <div className="console-note console-note-action" data-legacy-import>
            <FileInput aria-hidden="true" size={14} />
            <span><strong>{t("files.detectedTitle")}</strong> {t("files.detectedDescription", { name: legacy.name })}</span>
            <button className="button primary" disabled={busy !== null} onClick={() => void model.importFrom(legacy.path)} type="button">{busy === "import" ? t("files.importing") : t("files.importDetected")}</button>
          </div>
        )}
        <div className="console-table" role="table" aria-label={t("profiles.select")}>
          <div className="console-table-head" role="row"><span role="columnheader">{t("files.columnName")}</span><span role="columnheader">{t("files.columnFile")}</span><span role="columnheader">{t("files.columnState")}</span></div>
          {visible.map((entry) => {
            const selected = profile?.path === entry.path;
            const isApplied = matchesAppliedProfile(entry, appliedProfile);
            return (
              <button aria-pressed={selected} className="console-table-row" data-applied={isApplied} data-selected={selected} disabled={busy !== null} key={entry.path} onClick={() => void model.chooseProfile(entry.path)} onDoubleClick={() => void model.editInTuner(entry.path)} role="row" type="button">
                <strong role="cell">{entry.name}</strong>
                <code role="cell" title={entry.path}>{entry.displayPath}</code>
                <span role="cell">{isApplied && <span className="console-tag ok">{t("files.appliedBadge")}</span>}</span>
              </button>
            );
          })}
        </div>
      </ConsolePanel>

      <ConsolePanel
        footer={<>
          <button className="button secondary" disabled={!profile || busy !== null} onClick={() => profile && void model.editInTuner(profile.path)} type="button"><SlidersHorizontal aria-hidden="true" size={14} /> {t("files.editInTuner")}</button>
          <button className="button ghost" disabled={!profile || busy !== null} onClick={() => void model.revealCurrentProfile()} type="button"><FolderOpen aria-hidden="true" size={14} /> {t("files.reveal")}</button>
          <span className="console-spacer" />
          <button className="button secondary" disabled={!model.canSave} onClick={() => void model.save()} type="button"><Save aria-hidden="true" size={14} /> {busy === "save" ? t("profiles.saving") : t("profiles.save")}</button>
          <button className="button primary" disabled={!model.canApply} onClick={() => void model.apply()} title={model.dirtyCount > 0 ? t("profiles.saveBeforeApply") : undefined} type="button"><Play aria-hidden="true" size={14} strokeWidth={2} /> {busy === "apply" ? t("profiles.applying") : t("profiles.apply")}</button>
        </>}
        right={applied && <span className="console-tag ok">{t("files.appliedBadge")}</span>}
        scroll={false}
        title={t("files.selectedTitle")}
      >
        <SpecimenBoard className="specimen-board console-canvas console-canvas-fixed" dark={shell.theme === "dark"} fontFace="Segoe UI" profilePath={profile?.path ?? null} sizes={SPECIMEN_SIZES} text={t("profiles.samplePangram")} />
        <ConsoleKv rows={[
          { key: "file", label: t("files.columnFile"), value: <code title={profile?.path}>{profile?.displayPath ?? t("profiles.none")}</code> },
          { key: "unsaved", label: t("files.unsaved"), value: model.unsavedText },
          { key: "encoding", label: t("files.encoding"), value: model.encodingText },
        ]} />
        <div className="console-spacer" />
        <div className="console-saveas">
          <input aria-label={t("profiles.copyName")} className="console-field" disabled={!profile || busy !== null} onChange={(event) => model.setCopyName(event.target.value)} placeholder={t("files.saveAsName")} value={model.copyName} />
          <button className="button secondary" disabled={!model.canDuplicate} onClick={() => void model.duplicate()} type="button"><SaveAll aria-hidden="true" size={14} /> {t("files.saveAs")}</button>
        </div>
      </ConsolePanel>
    </ConsoleFrame>
  );
}
