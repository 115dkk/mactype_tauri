import type { SkinPreference } from "../../app/skinPreference";
import type { ThemePreference } from "../../app/themePreference";
import { nativePreviewChromeDefaults } from "../../generated/nativePreview";

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

export function nativeChrome(skin: SkinPreference, theme: ThemePreference): NativePreviewChrome {
  const contract = nativePreviewChromeDefaults.skins[skin];
  return { skin, ...contract.palettes[theme], ...contract.metrics };
}

export function currentSkin(): SkinPreference {
  const value = document.documentElement.dataset.skin;
  return value === "fluent" || value === "console" || value === "cupertino" ? value : "classic";
}
