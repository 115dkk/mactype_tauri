import type { PreviewEngine } from "../app/model";
import type { MessageKey } from "../i18n/i18n";
import { specimenPalette } from "../features/preview/specimenPalette";
import { specimenStripHeight, type SpecimenRequest } from "../features/preview/useSpecimenRenders";

export const studioStorageKey = "mactype-control-center.studio";
export const MAX_STUDIO_FONTS = 6;
export const STUDIO_SIZE_LADDER = [8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24, 28, 36] as const;
export const STUDIO_ZOOMS = [1, 2, 3, 4, 8] as const;
export const STUDIO_DPIS = [96, 120, 144, 192] as const;

export type StudioZoom = (typeof STUDIO_ZOOMS)[number];
export type StudioDpi = (typeof STUDIO_DPIS)[number];
export type StudioPreset = "pangramKo" | "pangramEn" | "mixed" | "digits" | "code" | "paragraph" | "custom";
export type StudioStyle = "regular" | "bold" | "italic" | "boldItalic";
export type StudioPaletteMode = "theme" | "custom";
export type StudioCompare = "side" | "stack" | "blink" | "diff";
export type StudioSourceKind = "none" | "edited" | "saved" | "profile" | "plain";

export interface StudioSource {
  kind: StudioSourceKind;
  /* For `profile` and `plain`: the profile file to render (plain ignores it but the helper needs a path for the mactype slot). */
  profilePath: string | null;
}

export interface StudioSettings {
  fonts: string[];
  sizes: number[];
  preset: StudioPreset;
  customText: string;
  styles: StudioStyle[];
  palette: StudioPaletteMode;
  /* Flips the theme-following polarity; ignored for custom colours. */
  inverted: boolean;
  foreground: string;
  background: string;
  sourceA: StudioSource;
  sourceB: StudioSource;
  compare: StudioCompare;
  zoom: StudioZoom;
  dpi: StudioDpi;
  follow: boolean;
}

/* The document the main window publishes to the studio. */
export interface StudioDocument {
  profilePath: string | null;
  profileName: string | null;
  values: Record<string, number>;
  savedValues: Record<string, number>;
  fontFace: string;
}

export const presetTexts: Readonly<Record<Exclude<StudioPreset, "custom">, string>> = {
  pangramKo: "다람쥐 헌 쳇바퀴에 타고파\n키스의 고유 조건은 입술끼리 만나야 하고 특별한 기술은 필요치 않다.",
  pangramEn: "The quick brown fox jumps over the lazy dog.\nSphinx of black quartz, judge my vow. 0123456789",
  mixed: "MacType 프리뷰 123 ABC · 다람쥐 헌 쳇바퀴에 타고파 · The quick brown fox jumps over the lazy dog.",
  digits: "0123456789 !@#$%^&*() []{}<>/\\|~`'\" ;:,.?-_=+ ¿¡ €£¥₩ ±×÷≠≈∞ ←↑→↓",
  code: "fn main() { let x: u32 = 0xFF; println!(\"{x:#?}\"); } // il1|O0 {}[]()<>",
  paragraph: "글꼴 렌더링은 힌팅, 안티앨리어싱, 감마와 대비가 함께 만들어 냅니다. 같은 글꼴이라도 설정에 따라 획의 굵기와 선명도가 달라지므로, 실제로 쓰는 크기에서 견본을 비교해야 합니다.\nRendering is the product of hinting, anti-aliasing, gamma and contrast together; compare specimens at the sizes you actually read.",
};

export const presetLabelKeys: Readonly<Record<StudioPreset, MessageKey>> = {
  pangramKo: "studio.preset.pangramKo",
  pangramEn: "studio.preset.pangramEn",
  mixed: "studio.preset.mixed",
  digits: "studio.preset.digits",
  code: "studio.preset.code",
  paragraph: "studio.preset.paragraph",
  custom: "studio.preset.custom",
};

export const styleLabelKeys: Readonly<Record<StudioStyle, MessageKey>> = {
  regular: "studio.style.regular",
  bold: "studio.style.bold",
  italic: "studio.style.italic",
  boldItalic: "studio.style.boldItalic",
};

export const sourceKindLabelKeys: Readonly<Record<StudioSourceKind, MessageKey>> = {
  none: "studio.source.none",
  edited: "studio.source.edited",
  saved: "studio.source.saved",
  profile: "studio.source.profile",
  plain: "studio.source.plain",
};

export const compareLabelKeys: Readonly<Record<StudioCompare, MessageKey>> = {
  side: "studio.compare.side",
  stack: "studio.compare.stack",
  blink: "studio.compare.blink",
  diff: "studio.compare.diff",
};

export function defaultStudioSettings(): StudioSettings {
  return {
    fonts: ["Segoe UI"],
    sizes: [9, 10, 11, 12, 14, 18, 24],
    preset: "mixed",
    customText: presetTexts.mixed,
    styles: ["regular"],
    palette: "theme",
    inverted: false,
    foreground: "#181D23",
    background: "#FFFFFF",
    sourceA: { kind: "edited", profilePath: null },
    sourceB: { kind: "plain", profilePath: null },
    compare: "side",
    zoom: 1,
    dpi: 96,
    follow: true,
  };
}

function isStringArray(value: unknown): value is string[] {
  return Array.isArray(value) && value.every((item) => typeof item === "string");
}

/* Restores the persisted settings, falling back field by field so a stale
   or hand-edited entry never blanks the studio. */
export function loadStudioSettings(): StudioSettings {
  const defaults = defaultStudioSettings();
  try {
    const raw = window.localStorage.getItem(studioStorageKey);
    if (!raw) return defaults;
    const parsed = JSON.parse(raw) as Partial<StudioSettings>;
    return {
      fonts: isStringArray(parsed.fonts) && parsed.fonts.length > 0 ? parsed.fonts.slice(0, MAX_STUDIO_FONTS) : defaults.fonts,
      sizes: Array.isArray(parsed.sizes) && parsed.sizes.every((size) => typeof size === "number" && size >= 6 && size <= 96) && parsed.sizes.length > 0 ? parsed.sizes : defaults.sizes,
      preset: parsed.preset && parsed.preset in presetLabelKeys ? parsed.preset : defaults.preset,
      customText: typeof parsed.customText === "string" ? parsed.customText : defaults.customText,
      styles: Array.isArray(parsed.styles) && parsed.styles.every((style) => style in styleLabelKeys) && parsed.styles.length > 0 ? parsed.styles : defaults.styles,
      palette: parsed.palette === "custom" ? "custom" : "theme",
      inverted: typeof parsed.inverted === "boolean" ? parsed.inverted : false,
      foreground: typeof parsed.foreground === "string" && /^#[0-9a-f]{6}$/i.test(parsed.foreground) ? parsed.foreground : defaults.foreground,
      background: typeof parsed.background === "string" && /^#[0-9a-f]{6}$/i.test(parsed.background) ? parsed.background : defaults.background,
      sourceA: parsed.sourceA && parsed.sourceA.kind in sourceKindLabelKeys ? { kind: parsed.sourceA.kind, profilePath: typeof parsed.sourceA.profilePath === "string" ? parsed.sourceA.profilePath : null } : defaults.sourceA,
      sourceB: parsed.sourceB && parsed.sourceB.kind in sourceKindLabelKeys ? { kind: parsed.sourceB.kind, profilePath: typeof parsed.sourceB.profilePath === "string" ? parsed.sourceB.profilePath : null } : defaults.sourceB,
      compare: parsed.compare && parsed.compare in compareLabelKeys ? parsed.compare : defaults.compare,
      zoom: STUDIO_ZOOMS.includes(parsed.zoom as StudioZoom) ? (parsed.zoom as StudioZoom) : defaults.zoom,
      dpi: STUDIO_DPIS.includes(parsed.dpi as StudioDpi) ? (parsed.dpi as StudioDpi) : defaults.dpi,
      follow: typeof parsed.follow === "boolean" ? parsed.follow : defaults.follow,
    };
  } catch {
    return defaults;
  }
}

export function saveStudioSettings(settings: StudioSettings): void {
  try {
    window.localStorage.setItem(studioStorageKey, JSON.stringify(settings));
  } catch {
    /* Storage can be unavailable in a locked-down profile; the studio still works for the session. */
  }
}

export function studioText(settings: StudioSettings): string {
  return settings.preset === "custom" ? settings.customText : presetTexts[settings.preset];
}

/* Custom colours win; otherwise the board follows the window theme, flipped
   by the invert switch. */
export function studioPalette(settings: StudioSettings, theme: "light" | "dark"): { foreground: string; background: string } {
  if (settings.palette === "custom") return { foreground: settings.foreground, background: settings.background };
  return specimenPalette((theme === "dark") !== settings.inverted);
}

/* The native window applies `inverted` itself, so it receives the theme's base
   polarity plus the flag; custom colours go through as they are. */
export function nativePalette(settings: StudioSettings, theme: "light" | "dark"): { foreground: string; background: string; inverted: boolean } {
  if (settings.palette === "custom") return { foreground: settings.foreground, background: settings.background, inverted: false };
  return { ...specimenPalette(theme === "dark"), inverted: settings.inverted };
}

export interface ResolvedSource {
  kind: StudioSourceKind;
  profilePath: string | null;
  overrides: Readonly<Record<string, number>>;
  engine: PreviewEngine;
  /* Why the source cannot render, when it cannot. */
  unavailable: "no-document" | "no-profile" | null;
}

/* Turns a source choice into what the helper needs: a profile path, the
   overrides, and the engine. Edited and saved need the Tuner document; a
   plain render borrows the document's profile path only so the request is
   well-formed. */
export function resolveSource(source: StudioSource, document: StudioDocument | null, fallbackProfile: string | null): ResolvedSource {
  const documentPath = document?.profilePath ?? null;
  switch (source.kind) {
    case "none":
      return { kind: "none", profilePath: null, overrides: {}, engine: "mactype", unavailable: null };
    case "edited":
      return { kind: "edited", profilePath: documentPath, overrides: document?.values ?? {}, engine: "mactype", unavailable: document ? null : "no-document" };
    case "saved":
      return { kind: "saved", profilePath: documentPath, overrides: document?.savedValues ?? {}, engine: "mactype", unavailable: document ? null : "no-document" };
    case "profile": {
      const path = source.profilePath ?? documentPath ?? fallbackProfile;
      return { kind: "profile", profilePath: path, overrides: {}, engine: "mactype", unavailable: path ? null : "no-profile" };
    }
    case "plain": {
      const path = source.profilePath ?? documentPath ?? fallbackProfile ?? "";
      return { kind: "plain", profilePath: path, overrides: {}, engine: "plain", unavailable: null };
    }
  }
}

export interface StudioStrip {
  key: string;
  font: string;
  size: number;
  style: StudioStyle;
}

/* Every strip a board shows, in reading order: font, then style, then size. */
export function studioStrips(settings: StudioSettings): ReadonlyArray<StudioStrip> {
  const strips: StudioStrip[] = [];
  for (const font of settings.fonts) {
    for (const style of settings.styles) {
      for (const size of settings.sizes) {
        strips.push({ key: `${font}|${style}|${size}`, font, size, style });
      }
    }
  }
  return strips;
}

export function studioRequests(settings: StudioSettings, source: ResolvedSource, widthPx: number, sideKey: string, theme: "light" | "dark"): ReadonlyArray<SpecimenRequest> {
  if (source.kind === "none" || source.unavailable || widthPx < 96) return [];
  const text = studioText(settings);
  const palette = studioPalette(settings, theme);
  return studioStrips(settings).map((strip) => ({
    key: `${sideKey}|${strip.key}`,
    profilePath: source.profilePath ?? "",
    overrides: source.overrides,
    engine: source.engine,
    text,
    fontFace: strip.font,
    fontSizePt: strip.size,
    widthPx: Math.round((widthPx * settings.dpi) / 96),
    heightPx: specimenStripHeight(text, strip.size, settings.dpi),
    dpi: settings.dpi,
    foreground: palette.foreground,
    background: palette.background,
    bold: strip.style === "bold" || strip.style === "boldItalic",
    italic: strip.style === "italic" || strip.style === "boldItalic",
  }));
}
