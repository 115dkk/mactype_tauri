import { readPreference, writePreference } from "./persistedPreference";

export type SkinPreference = "classic" | "fluent" | "console" | "cupertino";

export const skinStorageKey = "mactype-control-center.skin";

export const skinOptions: ReadonlyArray<{ value: SkinPreference; labelKey: "app.skinClassic" | "app.skinFluent" | "app.skinConsole" | "app.skinCupertino" }> = [
  { value: "classic", labelKey: "app.skinClassic" },
  { value: "fluent", labelKey: "app.skinFluent" },
  { value: "console", labelKey: "app.skinConsole" },
  { value: "cupertino", labelKey: "app.skinCupertino" },
];

function isSkin(value: string | null): value is SkinPreference {
  return value === "classic" || value === "fluent" || value === "console" || value === "cupertino";
}

/* An explicit ?skin= wins and is persisted, exactly like ?lang=, so the
   browser gallery can open every skin without touching stored preferences
   first; otherwise the stored choice applies and the classic skin is the
   default. */
export function loadSkinPreference(): SkinPreference {
  return readPreference<SkinPreference>({
    key: skinStorageKey,
    query: "skin",
    parse: (raw) => isSkin(raw) ? raw : null,
    fallback: () => "classic",
  });
}

export function applySkinPreference(skin: SkinPreference) {
  document.documentElement.dataset.skin = skin;
  writePreference(skinStorageKey, skin);
}
