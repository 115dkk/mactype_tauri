/* Bold substitution: what a substituted font draws when a bold face is asked
   for. The mode lives in the schema setting font_substitute_bold_mode; the
   explicit pairs live in AdvancedProfile.fontSubstituteBoldPairs as
   "ReplacementFamily=BoldFamily" lines. The Rust and renderer sides read the
   same shape, so the rules here must stay in step with theirs. */

export const BOLD_MODE_SETTING_ID = "font_substitute_bold_mode";
export const BOLD_MODE_PAIRS = 3;

const charsetSuffix = /,\d+$/;

function replacementOf(mapping: string): string | null {
  const separator = mapping.indexOf("=");
  if (separator < 0) return null;
  const replacement = mapping.slice(separator + 1).trim().replace(charsetSuffix, "").trim();
  return replacement || null;
}

function splitPair(pair: string): { family: string; bold: string } | null {
  const separator = pair.indexOf("=");
  if (separator < 0) return null;
  const family = pair.slice(0, separator).trim();
  const bold = pair.slice(separator + 1).trim();
  return family && bold ? { family, bold } : null;
}

/* The replacement families of the substitution mappings, in first-appearance
   order, keeping the first spelling of families that differ only in case. */
export function replacementFamilies(fontSubstitutes: ReadonlyArray<string>): string[] {
  const seen = new Set<string>();
  const families: string[] = [];
  for (const mapping of fontSubstitutes) {
    const family = replacementOf(mapping);
    if (!family) continue;
    const key = family.toLocaleLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    families.push(family);
  }
  return families;
}

/* The bold family paired with a replacement family, or null for none. The
   last pair written for a family wins, as an ini key would. */
export function boldPairFor(pairs: ReadonlyArray<string>, family: string): string | null {
  const key = family.trim().toLocaleLowerCase();
  let bold: string | null = null;
  for (const entry of pairs) {
    const pair = splitPair(entry);
    if (pair && pair.family.toLocaleLowerCase() === key) bold = pair.bold;
  }
  return bold;
}

/* Replaces the pair of one family; null or an empty bold family removes it. */
export function withBoldPair(pairs: ReadonlyArray<string>, family: string, boldFamily: string | null): string[] {
  const key = family.trim().toLocaleLowerCase();
  const kept = pairs.filter((entry) => splitPair(entry)?.family.toLocaleLowerCase() !== key);
  const bold = boldFamily?.trim() ?? "";
  return bold && family.trim() ? [...kept, `${family.trim()}=${bold}`] : kept;
}

/* The pairs the current mapping rows still own, one line per row in row
   order. Pairs whose family is no longer a replacement are dropped, and a
   line with an empty side is never written. */
export function rebuildBoldPairs(fontSubstitutes: ReadonlyArray<string>, pairs: ReadonlyArray<string>): string[] {
  return replacementFamilies(fontSubstitutes).flatMap((family) => {
    const bold = boldPairFor(pairs, family);
    return bold ? [`${family}=${bold}`] : [];
  });
}

/* The pair editor belongs to substitution being on and the pairs mode. */
export function boldPairsRequired(values: Readonly<Record<string, number>>): boolean {
  return (values.font_substitutes ?? 0) !== 0 && values[BOLD_MODE_SETTING_ID] === BOLD_MODE_PAIRS;
}

/* The bold mode means nothing while substitution is off, so every settings
   list hides it until substitution is chosen. */
export function isSettingDisclosed(settingId: string, values: Readonly<Record<string, number>>): boolean {
  if (settingId === BOLD_MODE_SETTING_ID) return (values.font_substitutes ?? 0) !== 0;
  return true;
}
