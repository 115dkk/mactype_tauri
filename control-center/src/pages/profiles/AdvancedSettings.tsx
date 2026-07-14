import { ArrowRight, Plus, Trash2 } from "lucide-react";
import type { AdvancedProfile } from "../../app/model";
import type { SettingDefinition } from "../../generated/settings";
import type { I18nValue } from "../../i18n/i18n";
import { SchemaSettings } from "./SchemaSettings";
import { splitSubstitution } from "./profileEditorUtils";

interface AdvancedSettingsProps {
  settings: ReadonlyArray<SettingDefinition>;
  values: Readonly<Record<string, number>>;
  dirtyKeys: ReadonlyArray<string>;
  advanced: AdvancedProfile;
  fontFamilies: ReadonlyArray<string>;
  fontOptionLabel: (font: string) => string;
  onSettingChange: (settingId: string, value: number) => void;
  onAdvancedChange: (profile: AdvancedProfile) => void;
  onAdvancedCommit: (profile: AdvancedProfile) => void;
  t: I18nValue["t"];
}

const updateVector = (values: ReadonlyArray<number>, index: number, value: number) =>
  values.map((current, currentIndex) => currentIndex === index ? value : current);

const colorValue = (value: number) => `#${value.toString(16).padStart(6, "0")}`;

export function AdvancedSettings({
  settings,
  values,
  dirtyKeys,
  advanced,
  fontFamilies,
  fontOptionLabel,
  onSettingChange,
  onAdvancedChange,
  onAdvancedCommit,
  t,
}: AdvancedSettingsProps) {
  const updateSubstitution = (index: number, key: "source" | "replacement", value: string) => {
    const substitutions = advanced.fontSubstitutes.map((mapping, currentIndex) => {
      if (currentIndex !== index) return mapping;
      const pair = splitSubstitution(mapping);
      return `${key === "source" ? value : pair.source}=${key === "replacement" ? value : pair.replacement}`;
    });
    onAdvancedCommit({ ...advanced, fontSubstitutes: substitutions });
  };

  const addSubstitution = () => {
    if (fontFamilies.length < 2) return;
    const used = new Set(advanced.fontSubstitutes.map((mapping) => splitSubstitution(mapping).source.toLocaleLowerCase()));
    const source = fontFamilies.find((font) => !used.has(font.toLocaleLowerCase())) ?? fontFamilies[0];
    const replacement = fontFamilies.find((font) => font !== source && font === "Segoe UI")
      ?? fontFamilies.find((font) => font !== source)
      ?? source;
    onAdvancedCommit({ ...advanced, fontSubstitutes: [...advanced.fontSubstitutes, `${source}=${replacement}`] });
  };

  const removeSubstitution = (index: number) => {
    onAdvancedCommit({ ...advanced, fontSubstitutes: advanced.fontSubstitutes.filter((_, currentIndex) => currentIndex !== index) });
  };

  return (
    <>
      <SchemaSettings dirtyKeys={dirtyKeys} onChange={onSettingChange} settings={settings} t={t} values={values} />
      <div className="advanced-editor">
        <fieldset>
          <legend>{t("advanced.shadow")}</legend><p>{t("advanced.shadowHelp")}</p>
          <label className="checkbox-row"><input checked={advanced.shadow !== null} onChange={(event) => onAdvancedCommit({ ...advanced, shadow: event.target.checked ? { offsetX: 1, offsetY: 1, darkAlpha: 4, darkColor: 0, lightAlpha: 4, lightColor: 0 } : null })} type="checkbox" /> {t("advanced.enableCustom")}</label>
          {advanced.shadow && <div className="advanced-grid">{(["offsetX", "offsetY", "darkAlpha", "lightAlpha"] as const).map((key) => <label key={key}><span>{t(`advanced.${key}`)}</span><input max={key.includes("Alpha") ? 255 : 128} min={key.includes("Alpha") ? 0 : -128} onChange={(event) => onAdvancedChange({ ...advanced, shadow: { ...advanced.shadow!, [key]: Number(event.target.value) } })} onBlur={() => onAdvancedCommit(advanced)} type="number" value={advanced.shadow?.[key] ?? 0} /></label>)}{(["darkColor", "lightColor"] as const).map((key) => <label key={key}><span>{t(`advanced.${key}`)}</span><input onChange={(event) => onAdvancedCommit({ ...advanced, shadow: { ...advanced.shadow!, [key]: Number.parseInt(event.target.value.slice(1), 16) } })} type="color" value={colorValue(advanced.shadow?.[key] ?? 0)} /></label>)}</div>}
        </fieldset>
        <fieldset>
          <legend>{t("advanced.lcdWeight")}</legend><p>{t("advanced.lcdWeightHelp")}</p>
          <label className="checkbox-row"><input checked={advanced.lcdFilterWeight !== null} onChange={(event) => onAdvancedCommit({ ...advanced, lcdFilterWeight: event.target.checked ? [8, 77, 86, 77, 8] : null })} type="checkbox" /> {t("advanced.enableCustom")}</label>
          {advanced.lcdFilterWeight && <div className="vector-grid">{advanced.lcdFilterWeight.map((value, index) => <label key={index}><span>{index + 1}</span><input max={255} min={0} onChange={(event) => onAdvancedChange({ ...advanced, lcdFilterWeight: updateVector(advanced.lcdFilterWeight!, index, Number(event.target.value)) })} onBlur={() => onAdvancedCommit(advanced)} type="number" value={value} /></label>)}</div>}
        </fieldset>
        <fieldset>
          <legend>{t("advanced.pixelLayout")}</legend><p>{t("advanced.pixelLayoutHelp")}</p>
          <label className="checkbox-row"><input checked={advanced.pixelLayout !== null} onChange={(event) => onAdvancedCommit({ ...advanced, pixelLayout: event.target.checked ? [-21, 0, 0, 0, 21, 0] : null })} type="checkbox" /> {t("advanced.enableCustom")}</label>
          {advanced.pixelLayout && <div className="vector-grid">{advanced.pixelLayout.map((value, index) => <label key={index}><span>{["R x", "R y", "G x", "G y", "B x", "B y"][index]}</span><input max={127} min={-128} onChange={(event) => onAdvancedChange({ ...advanced, pixelLayout: updateVector(advanced.pixelLayout!, index, Number(event.target.value)) })} onBlur={() => onAdvancedCommit(advanced)} type="number" value={value} /></label>)}</div>}
        </fieldset>
        <fieldset className="advanced-text-fields">
          <legend>{t("advanced.routing")}</legend>
          <label><span>{t("advanced.displayAffinity")}</span><small>{t("advanced.displayAffinityHelp")}</small><input onChange={(event) => onAdvancedChange({ ...advanced, displayAffinity: event.target.value.split(",").map((part) => Number(part.trim())).filter(Number.isInteger) })} onBlur={() => onAdvancedCommit(advanced)} type="text" value={advanced.displayAffinity.join(", ")} /></label>
          <div className="font-substitution-editor">
            <strong>{t("advanced.fontSubstitutes")}</strong>
            <small>{t("advanced.fontSubstitutesHelp")}</small>
            <div className="font-substitution-list">
              {advanced.fontSubstitutes.map((mapping, index) => {
                const pair = splitSubstitution(mapping);
                return (
                  <div className="font-substitution-row" key={`${mapping}-${index}`}>
                    <label><span className="sr-only">{t("profiles.sourceFont")}</span><select aria-label={t("profiles.sourceFont")} onChange={(event) => updateSubstitution(index, "source", event.target.value)} value={pair.source}>{fontFamilies.map((font) => <option key={font} value={font}>{fontOptionLabel(font)}</option>)}</select></label>
                    <ArrowRight aria-hidden="true" size={16} />
                    <label><span className="sr-only">{t("profiles.replacementFont")}</span><select aria-label={t("profiles.replacementFont")} onChange={(event) => updateSubstitution(index, "replacement", event.target.value)} value={pair.replacement}>{fontFamilies.map((font) => <option key={font} value={font}>{fontOptionLabel(font)}</option>)}</select></label>
                    <button className="icon-button" aria-label={t("profiles.remove", { name: pair.source })} onClick={() => removeSubstitution(index)} type="button"><Trash2 aria-hidden="true" size={15} /></button>
                  </div>
                );
              })}
            </div>
            <button className="button secondary font-add-button" disabled={fontFamilies.length < 2} onClick={addSubstitution} type="button"><Plus aria-hidden="true" size={16} /> {t("profiles.addSubstitution")}</button>
          </div>
        </fieldset>
        <fieldset>
          <legend>Infinality</legend>
          <p>{t("advanced.infinalityVectorsHelp")}</p>
          <div className="vector-grid">{advanced.infinalityGammaCorrection.map((value, index) => <label key={`gamma-${index}`}><span>{t("advanced.gammaCorrection")} {index + 1}</span><input onChange={(event) => onAdvancedChange({ ...advanced, infinalityGammaCorrection: updateVector(advanced.infinalityGammaCorrection, index, Number(event.target.value)) })} onBlur={() => onAdvancedCommit(advanced)} type="number" value={value} /></label>)}{advanced.infinalityFilterParams.map((value, index) => <label key={`filter-${index}`}><span>{t("advanced.filterParams")} {index + 1}</span><input max={255} min={0} onChange={(event) => onAdvancedChange({ ...advanced, infinalityFilterParams: updateVector(advanced.infinalityFilterParams, index, Number(event.target.value)) })} onBlur={() => onAdvancedCommit(advanced)} type="number" value={value} /></label>)}</div>
        </fieldset>
      </div>
    </>
  );
}
