import { ArrowLeft, ArrowRight, Eye, Play, Save } from "lucide-react";
import type { AdvancedProfile } from "../../app/model";
import type { SettingDefinition } from "../../generated/settings";
import type { I18nValue } from "../../i18n/i18n";
import { FontSubstitutionEditor } from "./FontSubstitutionEditor";
import { SchemaSettings } from "./SchemaSettings";
import { wizardSettingIdsByStep, wizardStepIds, type WizardStepId } from "./wizardModel";

interface WizardSettingsProps {
  activeStep: WizardStepId;
  advanced: AdvancedProfile;
  busy: boolean;
  canSave: boolean;
  dirtyCount: number;
  dirtyKeys: ReadonlyArray<string>;
  fontFamilies: ReadonlyArray<string>;
  fontOptionLabel: (font: string) => string;
  onAdvancedCommit: (profile: AdvancedProfile) => void;
  onApply: () => void;
  onPreview: () => void;
  onSave: () => void;
  onSettingChange: (settingId: string, value: number) => void;
  onSettingPreview: (settingId: string, value: number) => void;
  onStepChange: (step: WizardStepId) => void;
  savedValues?: Readonly<Record<string, number>>;
  settings: ReadonlyArray<SettingDefinition>;
  t: I18nValue["t"];
  values: Readonly<Record<string, number>>;
}

export function WizardSettings({
  activeStep,
  advanced,
  busy,
  canSave,
  dirtyCount,
  dirtyKeys,
  fontFamilies,
  fontOptionLabel,
  onAdvancedCommit,
  onApply,
  onPreview,
  onSave,
  onSettingChange,
  onSettingPreview,
  onStepChange,
  savedValues,
  settings,
  t,
  values,
}: WizardSettingsProps) {
  const stepIndex = wizardStepIds.indexOf(activeStep);
  const currentSettings = settings.filter((setting) => wizardSettingIdsByStep[activeStep].includes(setting.id));
  const previousStep = wizardStepIds[stepIndex - 1];
  const nextStep = wizardStepIds[stepIndex + 1];

  return (
    <div className="wizard-layout">
      <div className="wizard-step-content">
        {activeStep !== "apply" && <SchemaSettings dirtyKeys={dirtyKeys} onChange={onSettingChange} onPreviewChange={onSettingPreview} savedValues={savedValues} settings={currentSettings} t={t} values={values} />}
        {activeStep === "substitution" && (
          <div className="advanced-editor wizard-substitution">
            <fieldset><FontSubstitutionEditor advanced={advanced} fontFamilies={fontFamilies} fontOptionLabel={fontOptionLabel} onCommit={onAdvancedCommit} t={t} /></fieldset>
          </div>
        )}
        {activeStep === "apply" && (
          <section className="wizard-apply-card" aria-label={t("wizard.apply")}>
            <p data-dirty={dirtyCount > 0}>{dirtyCount > 0 ? t("wizard.unsavedWarning") : t("wizard.savedState")}</p>
            <div>
              <button className="button secondary" onClick={onPreview} type="button"><Eye aria-hidden="true" size={16} /> {t("profiles.preview")}</button>
              <button className="button secondary" disabled={busy || dirtyCount === 0 || !canSave} onClick={onSave} type="button"><Save aria-hidden="true" size={16} /> {t("wizard.saveProfile")}</button>
              <button className="button primary" disabled={busy || dirtyCount > 0} onClick={onApply} title={dirtyCount > 0 ? t("profiles.saveBeforeApply") : undefined} type="button"><Play aria-hidden="true" size={16} /> {t("wizard.applyMacType")}</button>
            </div>
          </section>
        )}
      </div>
      <nav className="wizard-progress" aria-label={t("wizard.progress")}>
        {previousStep && <button className="button secondary" onClick={() => onStepChange(previousStep)} type="button"><ArrowLeft aria-hidden="true" size={16} /> {t("wizard.previous")}</button>}
        {nextStep && <button className="button primary wizard-next" onClick={() => onStepChange(nextStep)} type="button">{t("wizard.next")} <ArrowRight aria-hidden="true" size={16} /></button>}
      </nav>
    </div>
  );
}
