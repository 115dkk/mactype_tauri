export type WizardStepId = "rendering" | "quality" | "hinting" | "gamma" | "lcd" | "substitution" | "apply";

export const wizardStepIds: ReadonlyArray<WizardStepId> = ["rendering", "quality", "hinting", "gamma", "lcd", "substitution", "apply"];

export const wizardSettingIdsByStep: Readonly<Record<WizardStepId, ReadonlyArray<string>>> = {
  rendering: ["anti_alias_mode"],
  quality: ["normal_weight", "bold_weight", "contrast", "render_weight", "enable_kerning"],
  hinting: ["hinting_mode", "hint_small_font"],
  gamma: ["gamma_mode", "gamma_value"],
  lcd: ["lcd_filter", "text_tuning"],
  substitution: ["font_substitutes"],
  apply: [],
};

export const wizardSettingIds = [...new Set(Object.values(wizardSettingIdsByStep).flat())];
