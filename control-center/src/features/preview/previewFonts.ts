import type { Locale } from "../../i18n/i18n";
import { scriptUiFont } from "./scriptUiFont";

const familyName = (value: string) => value.trim().replace(/^"(.*)"$/, "$1").trim();
const aliases: ReadonlyArray<ReadonlyArray<string>> = [
  ["Malgun Gothic", "맑은 고딕"],
  ["Yu Gothic UI", "游ゴシック UI"],
  ["Microsoft YaHei UI", "微软雅黑 UI"],
  ["Microsoft JhengHei UI", "微軟正黑體 UI"],
];

function familyKey(value: string): string {
  const key = familyName(value).toLowerCase();
  return aliases.find((group) => group.some((name) => name.toLowerCase() === key))?.[0].toLowerCase() ?? key;
}

export function substitutedPreviewFont(source: string, mappings: ReadonlyArray<string>): string {
  const replacements = new Set(mappings.flatMap((mapping) => {
    const separator = mapping.indexOf("=");
    if (separator < 0 || familyKey(mapping.slice(0, separator)) !== familyKey(source)) return [];
    const replacement = familyName(mapping.slice(separator + 1));
    return replacement ? [replacement] : [];
  }));
  // Conflicting localized aliases cannot establish one replacement family.
  return replacements.size === 1 ? [...replacements][0] : source;
}

export function previewFontOptions(locale: Locale, mappings: ReadonlyArray<string>) {
  const script = scriptUiFont(locale);
  return ["Segoe UI", ...(script ? [script] : [])].map((source) => ({
    value: source,
    label: substitutedPreviewFont(source, mappings),
  }));
}
