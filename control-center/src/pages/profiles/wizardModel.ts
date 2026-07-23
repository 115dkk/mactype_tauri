export type WizardStepId = "start" | "rendering" | "quality" | "hinting" | "gamma" | "lcd" | "substitution" | "apply";

export const wizardStepIds: ReadonlyArray<WizardStepId> = ["start", "rendering", "quality", "hinting", "gamma", "lcd", "substitution", "apply"];

export const wizardSettingIdsByStep: Readonly<Record<WizardStepId, ReadonlyArray<string>>> = {
  start: [],
  rendering: ["anti_alias_mode"],
  quality: ["normal_weight", "bold_weight", "contrast", "render_weight", "enable_kerning"],
  hinting: ["hinting_mode", "hint_small_font"],
  gamma: ["gamma_mode", "gamma_value"],
  lcd: ["lcd_filter", "text_tuning"],
  substitution: ["font_substitutes"],
  apply: [],
};

export const wizardSettingIds = [...new Set(Object.values(wizardSettingIdsByStep).flat())];

export type WizardScaleId = "weight" | "contrast" | "gamma";

/* Guided sliders speak in outcomes, not numbers, following the legacy
   MacType Tuner endpoints (Thin↔Thick, Low↔High, Dark↔Light). */
export const wizardScaleBySettingId: Readonly<Record<string, WizardScaleId>> = {
  normal_weight: "weight",
  bold_weight: "weight",
  render_weight: "weight",
  contrast: "contrast",
  gamma_value: "gamma",
};
