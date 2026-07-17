import type { AdvancedProfile } from "../../app/model";
import type { SettingDefinition } from "../../generated/settings";
import type { I18nValue } from "../../i18n/i18n";
import { FontSubstitutionEditor } from "./FontSubstitutionEditor";
import { SchemaSettings } from "./SchemaSettings";

interface AdvancedSettingsProps {
  settings: ReadonlyArray<SettingDefinition>;
  values: Readonly<Record<string, number>>;
  dirtyKeys: ReadonlyArray<string>;
  advanced: AdvancedProfile;
  fontFamilies: ReadonlyArray<string>;
  fontOptionLabel: (font: string) => string;
  onSettingChange: (settingId: string, value: number) => void;
  onSettingPreview: (settingId: string, value: number) => void;
  onAdvancedChange: (profile: AdvancedProfile) => void;
  onAdvancedCommit: (profile: AdvancedProfile) => void;
  t: I18nValue["t"];
}

interface AdvancedSectionProps {
  advanced: AdvancedProfile;
  onChange: (profile: AdvancedProfile) => void;
  onCommit: (profile: AdvancedProfile) => void;
  t: I18nValue["t"];
}

const updateVector = (values: ReadonlyArray<number>, index: number, value: number) =>
  values.map((current, currentIndex) => currentIndex === index ? value : current);

const colorValue = (value: number) => `#${value.toString(16).padStart(6, "0")}`;

function ShadowSettings({ advanced, onChange, onCommit, t }: AdvancedSectionProps) {
  const shadow = advanced.shadow;
  const numberFields = ["offsetX", "offsetY", "darkAlpha", "lightAlpha"] as const;
  const colorFields = ["darkColor", "lightColor"] as const;

  return (
    <fieldset>
      <legend>{t("advanced.shadow")}</legend>
      <p>{t("advanced.shadowHelp")}</p>
      <label className="checkbox-row">
        <input
          checked={shadow !== null}
          onChange={(event) => onCommit({
            ...advanced,
            shadow: event.target.checked
              ? { offsetX: 1, offsetY: 1, darkAlpha: 4, darkColor: 0, lightAlpha: 4, lightColor: 0 }
              : null,
          })}
          type="checkbox"
        /> {t("advanced.enableCustom")}
      </label>
      {shadow && (
        <div className="advanced-grid">
          {numberFields.map((key) => (
            <label key={key}>
              <span>{t(`advanced.${key}`)}</span>
              <input
                max={key.includes("Alpha") ? 255 : 128}
                min={key.includes("Alpha") ? 0 : -128}
                onBlur={() => onCommit(advanced)}
                onChange={(event) => onChange({ ...advanced, shadow: { ...shadow, [key]: Number(event.target.value) } })}
                type="number"
                value={shadow[key]}
              />
            </label>
          ))}
          {colorFields.map((key) => (
            <label key={key}>
              <span>{t(`advanced.${key}`)}</span>
              <input
                onChange={(event) => onCommit({ ...advanced, shadow: { ...shadow, [key]: Number.parseInt(event.target.value.slice(1), 16) } })}
                type="color"
                value={colorValue(shadow[key])}
              />
            </label>
          ))}
        </div>
      )}
    </fieldset>
  );
}

function LcdFilterSettings({ advanced, onChange, onCommit, t }: AdvancedSectionProps) {
  const weights = advanced.lcdFilterWeight;
  return (
    <fieldset>
      <legend>{t("advanced.lcdWeight")}</legend>
      <p>{t("advanced.lcdWeightHelp")}</p>
      <label className="checkbox-row">
        <input
          checked={weights !== null}
          onChange={(event) => onCommit({ ...advanced, lcdFilterWeight: event.target.checked ? [8, 77, 86, 77, 8] : null })}
          type="checkbox"
        /> {t("advanced.enableCustom")}
      </label>
      {weights && (
        <div className="vector-grid">
          {weights.map((value, index) => (
            <label key={index}>
              <span>{index + 1}</span>
              <input
                max={255}
                min={0}
                onBlur={() => onCommit(advanced)}
                onChange={(event) => onChange({ ...advanced, lcdFilterWeight: updateVector(weights, index, Number(event.target.value)) })}
                type="number"
                value={value}
              />
            </label>
          ))}
        </div>
      )}
    </fieldset>
  );
}

function PixelLayoutSettings({ advanced, onChange, onCommit, t }: AdvancedSectionProps) {
  const pixelLayout = advanced.pixelLayout;
  const labels = ["R x", "R y", "G x", "G y", "B x", "B y"];
  return (
    <fieldset>
      <legend>{t("advanced.pixelLayout")}</legend>
      <p>{t("advanced.pixelLayoutHelp")}</p>
      <label className="checkbox-row">
        <input
          checked={pixelLayout !== null}
          onChange={(event) => onCommit({ ...advanced, pixelLayout: event.target.checked ? [-21, 0, 0, 0, 21, 0] : null })}
          type="checkbox"
        /> {t("advanced.enableCustom")}
      </label>
      {pixelLayout && (
        <div className="vector-grid">
          {pixelLayout.map((value, index) => (
            <label key={labels[index]}>
              <span>{labels[index]}</span>
              <input
                max={127}
                min={-128}
                onBlur={() => onCommit(advanced)}
                onChange={(event) => onChange({ ...advanced, pixelLayout: updateVector(pixelLayout, index, Number(event.target.value)) })}
                type="number"
                value={value}
              />
            </label>
          ))}
        </div>
      )}
    </fieldset>
  );
}

function RoutingSettings({
  advanced,
  fontFamilies,
  fontOptionLabel,
  onChange,
  onCommit,
  t,
}: AdvancedSectionProps & Pick<AdvancedSettingsProps, "fontFamilies" | "fontOptionLabel">) {
  return (
    <fieldset className="advanced-text-fields">
      <legend>{t("advanced.routing")}</legend>
      <label>
        <span>{t("advanced.displayAffinity")}</span>
        <small>{t("advanced.displayAffinityHelp")}</small>
        <input
          onBlur={() => onCommit(advanced)}
          onChange={(event) => onChange({
            ...advanced,
            displayAffinity: event.target.value.split(",").map((part) => Number(part.trim())).filter(Number.isInteger),
          })}
          type="text"
          value={advanced.displayAffinity.join(", ")}
        />
      </label>
      <FontSubstitutionEditor advanced={advanced} fontFamilies={fontFamilies} fontOptionLabel={fontOptionLabel} onCommit={onCommit} t={t} />
    </fieldset>
  );
}

function InfinalitySettings({ advanced, onChange, onCommit, t }: AdvancedSectionProps) {
  return (
    <fieldset>
      <legend>Infinality</legend>
      <p>{t("advanced.infinalityVectorsHelp")}</p>
      <div className="vector-grid">
        {advanced.infinalityGammaCorrection.map((value, index) => (
          <label key={`gamma-${index}`}>
            <span>{t("advanced.gammaCorrection")} {index + 1}</span>
            <input
              onBlur={() => onCommit(advanced)}
              onChange={(event) => onChange({
                ...advanced,
                infinalityGammaCorrection: updateVector(advanced.infinalityGammaCorrection, index, Number(event.target.value)),
              })}
              type="number"
              value={value}
            />
          </label>
        ))}
        {advanced.infinalityFilterParams.map((value, index) => (
          <label key={`filter-${index}`}>
            <span>{t("advanced.filterParams")} {index + 1}</span>
            <input
              max={255}
              min={0}
              onBlur={() => onCommit(advanced)}
              onChange={(event) => onChange({
                ...advanced,
                infinalityFilterParams: updateVector(advanced.infinalityFilterParams, index, Number(event.target.value)),
              })}
              type="number"
              value={value}
            />
          </label>
        ))}
      </div>
    </fieldset>
  );
}

export function AdvancedSettings({
  settings,
  values,
  dirtyKeys,
  advanced,
  fontFamilies,
  fontOptionLabel,
  onSettingChange,
  onSettingPreview,
  onAdvancedChange,
  onAdvancedCommit,
  t,
}: AdvancedSettingsProps) {
  const sectionProps = { advanced, onChange: onAdvancedChange, onCommit: onAdvancedCommit, t };
  return (
    <>
      <SchemaSettings dirtyKeys={dirtyKeys} onChange={onSettingChange} onPreviewChange={onSettingPreview} settings={settings} t={t} values={values} />
      <div className="advanced-editor">
        <ShadowSettings {...sectionProps} />
        <LcdFilterSettings {...sectionProps} />
        <PixelLayoutSettings {...sectionProps} />
        <RoutingSettings {...sectionProps} fontFamilies={fontFamilies} fontOptionLabel={fontOptionLabel} />
        <InfinalitySettings {...sectionProps} />
      </div>
    </>
  );
}
