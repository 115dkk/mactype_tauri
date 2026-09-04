import { Palette } from "lucide-react";
import { skinOptions, type SkinPreference } from "../app/skinPreference";
import { useI18n } from "../i18n/i18n";
import { PreferenceMenu } from "./PreferenceMenu";

interface SkinPickerProps {
  skin: SkinPreference;
  onChange: (next: SkinPreference) => void;
}

export function SkinPicker({ skin, onChange }: SkinPickerProps) {
  const { t } = useI18n();

  return (
    <PreferenceMenu
      icon={<Palette aria-hidden="true" size={17} />}
      label={t("app.skin")}
      onChange={onChange}
      optionAttribute="data-skin-option"
      options={skinOptions.map((option) => ({ value: option.value, label: t(option.labelKey) }))}
      testId="skin-picker-trigger"
      value={skin}
    />
  );
}
