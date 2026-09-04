import type { SkinPreference } from "../../app/skinPreference";
import type { ThemePreference } from "../../app/themePreference";

/* The colours and metrics the native preview window draws its chrome with,
   computed from the active skin so the window matches the one that opened
   it. The helper knows nothing about skins; it only reads this object.
   Values mirror `docs/skin-designs.md`. */
export interface NativePreviewChrome {
  skin: SkinPreference;
  canvas: string;
  surface: string;
  surfaceSubtle: string;
  border: string;
  text: string;
  muted: string;
  accent: string;
  onAccent: string;
  radius: number;
  controlHeight: number;
  toolbarHeight: number;
  statusHeight: number;
  canvasRadius: number;
  canvasInset: number;
  monoStatus: boolean;
}

type Palette = Omit<NativePreviewChrome, "skin" | "radius" | "controlHeight" | "toolbarHeight" | "statusHeight" | "canvasRadius" | "canvasInset" | "monoStatus">;

const palettes: Record<SkinPreference, Record<ThemePreference, Palette>> = {
  classic: {
    light: { canvas: "#F3F5F7", surface: "#FFFFFF", surfaceSubtle: "#E9EDF1", border: "#C9D1D8", text: "#17212B", muted: "#5A6773", accent: "#0067C0", onAccent: "#FFFFFF" },
    dark: { canvas: "#11161B", surface: "#192027", surfaceSubtle: "#222B33", border: "#34414C", text: "#E8EDF2", muted: "#9AA8B5", accent: "#4CA6E8", onAccent: "#07131C" },
  },
  fluent: {
    light: { canvas: "#F0F2F5", surface: "#FBFBFC", surfaceSubtle: "#F4F5F7", border: "#E6E8EB", text: "#1B1B1B", muted: "#5F646B", accent: "#005FB8", onAccent: "#FFFFFF" },
    dark: { canvas: "#202124", surface: "#2B2C30", surfaceSubtle: "#323337", border: "#3A3B40", text: "#FFFFFF", muted: "#C4C8CE", accent: "#60CDFF", onAccent: "#06202C" },
  },
  console: {
    light: { canvas: "#E4E8EC", surface: "#F4F6F8", surfaceSubtle: "#FFFFFF", border: "#D2D8DF", text: "#1B2129", muted: "#5A6673", accent: "#0B8E9F", onAccent: "#FFFFFF" },
    dark: { canvas: "#15181C", surface: "#1C2025", surfaceSubtle: "#23282E", border: "#2B3138", text: "#DDE2E7", muted: "#8C97A3", accent: "#3FC8D8", onAccent: "#062A30" },
  },
  cupertino: {
    light: { canvas: "#F5F5F7", surface: "#FFFFFF", surfaceSubtle: "#F2F2F4", border: "#E3E3E6", text: "#1D1D1F", muted: "#6E6E73", accent: "#3B74D8", onAccent: "#FFFFFF" },
    dark: { canvas: "#1E1E1E", surface: "#2C2C2E", surfaceSubtle: "#3A3A3C", border: "#3F3F42", text: "#F5F5F7", muted: "#98989D", accent: "#4C8DF5", onAccent: "#FFFFFF" },
  },
};

const metrics: Record<SkinPreference, Pick<NativePreviewChrome, "radius" | "controlHeight" | "toolbarHeight" | "statusHeight" | "canvasRadius" | "canvasInset" | "monoStatus">> = {
  classic: { radius: 4, controlHeight: 32, toolbarHeight: 44, statusHeight: 28, canvasRadius: 4, canvasInset: 18, monoStatus: false },
  fluent: { radius: 4, controlHeight: 32, toolbarHeight: 48, statusHeight: 28, canvasRadius: 3, canvasInset: 18, monoStatus: false },
  console: { radius: 4, controlHeight: 26, toolbarHeight: 36, statusHeight: 24, canvasRadius: 4, canvasInset: 10, monoStatus: true },
  cupertino: { radius: 6, controlHeight: 26, toolbarHeight: 40, statusHeight: 26, canvasRadius: 10, canvasInset: 14, monoStatus: false },
};

export function nativeChrome(skin: SkinPreference, theme: ThemePreference): NativePreviewChrome {
  return { skin, ...palettes[skin][theme], ...metrics[skin] };
}

export function currentSkin(): SkinPreference {
  const value = document.documentElement.dataset.skin;
  return value === "fluent" || value === "console" || value === "cupertino" ? value : "classic";
}
