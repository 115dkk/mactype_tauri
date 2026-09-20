import { useState } from "react";
import { useAppTheme } from "../../app/useAppTheme";
import type { Locale } from "../../i18n/i18n";
import { useI18n } from "../../i18n/i18n";
import { previewFontOptions } from "./previewFonts";
import { scriptUiFont } from "./scriptUiFont";
import { usePreviewFontSubstitutes } from "./usePreviewFontSubstitutes";

interface OverviewSpecimenOptions {
  locale: Locale;
  appliedProfilePath: string | null;
  expectedProfileDigest?: string | null;
}

export function useOverviewSpecimen({ locale, appliedProfilePath, expectedProfileDigest }: OverviewSpecimenOptions) {
  const { t } = useI18n();
  const substitutes = usePreviewFontSubstitutes(appliedProfilePath, expectedProfileDigest);
  const fontOptions = previewFontOptions(locale, substitutes.mappings);
  const [fontSource, setFontSource] = useState<string | null>(null);
  const selectedFont = fontOptions.find((option) => option.value === (fontSource ?? scriptUiFont(locale))) ?? fontOptions[0];
  const theme = useAppTheme();
  const [inverted, setInverted] = useState(false);
  const [sample, setSample] = useState(() => t("profiles.samplePangram"));
  const [editing, setEditing] = useState(false);

  return {
    fontOptions,
    selectedFont,
    fontFace: selectedFont.label,
    setFontSource,
    inverted,
    setInverted,
    dark: (theme === "dark") !== inverted,
    sample,
    setSample,
    editing,
    setEditing,
    ready: substitutes.ready,
    error: substitutes.error,
  };
}
