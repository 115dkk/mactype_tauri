import { Languages } from "lucide-react";
import { localeOptions, useI18n } from "../i18n/i18n";
import { PreferenceMenu } from "./PreferenceMenu";

export function LanguagePicker() {
  const { locale, setLocale, t } = useI18n();

  return (
    <PreferenceMenu
      icon={<Languages aria-hidden="true" size={17} />}
      label={t("app.language")}
      onChange={setLocale}
      optionAttribute="data-locale-option"
      options={localeOptions.map((option) => ({ value: option.value, label: t(option.labelKey) }))}
      testId="language-picker-trigger"
      value={locale}
    />
  );
}
