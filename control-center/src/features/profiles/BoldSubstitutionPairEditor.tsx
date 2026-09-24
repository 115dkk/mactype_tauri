import { ArrowRight } from "lucide-react";
import type { AdvancedProfile } from "../../app/model";
import type { I18nValue } from "../../i18n/i18n";
import { boldPairFor, rebuildBoldPairs, replacementFamilies, withBoldPair } from "./boldSubstitution";

interface BoldSubstitutionPairEditorProps {
  advanced: AdvancedProfile;
  fontFamilies: ReadonlyArray<string>;
  fontOptionLabel: (font: string) => string;
  onCommit: (profile: AdvancedProfile) => void;
  t: I18nValue["t"];
}

/* One row per replacement family of the substitution mappings. The family
   side follows the mapping list and cannot be edited here; the bold side
   starts at none, which writes no pair. */
export function BoldSubstitutionPairEditor({ advanced, fontFamilies, fontOptionLabel, onCommit, t }: BoldSubstitutionPairEditorProps) {
  const families = replacementFamilies(advanced.fontSubstitutes);
  const pairs = advanced.fontSubstituteBoldPairs;

  if (families.length === 0) {
    return <p className="bold-substitution-empty" role="note">{t("profiles.boldPairsEmpty")}</p>;
  }

  const choose = (family: string, boldFamily: string) => {
    const next = rebuildBoldPairs(advanced.fontSubstitutes, withBoldPair(pairs, family, boldFamily || null));
    onCommit({ ...advanced, fontSubstituteBoldPairs: next });
  };

  return (
    <div className="font-substitution-editor bold-substitution-editor">
      <div className="font-substitution-list">
        {families.map((family) => {
          const bold = boldPairFor(pairs, family) ?? "";
          const options = bold && !fontFamilies.some((font) => font.toLocaleLowerCase() === bold.toLocaleLowerCase())
            ? [...fontFamilies, bold]
            : fontFamilies;
          return (
            <div className="font-substitution-row" data-bold-family={family} key={family.toLocaleLowerCase()}>
              <output aria-label={t("profiles.replacementFont")} className="font-substitution-source" title={family}>{family}</output>
              <ArrowRight aria-hidden="true" size={16} />
              <label>
                <span className="sr-only">{t("profiles.boldReplacementFont")}</span>
                <select aria-label={t("profiles.boldReplacementFont")} onChange={(event) => choose(family, event.target.value)} value={options.find((font) => font.toLocaleLowerCase() === bold.toLocaleLowerCase()) ?? ""}>
                  <option value="">{t("profiles.boldPairNone")}</option>
                  {options.map((font) => <option key={font} value={font}>{fontOptionLabel(font)}</option>)}
                </select>
              </label>
            </div>
          );
        })}
      </div>
    </div>
  );
}
