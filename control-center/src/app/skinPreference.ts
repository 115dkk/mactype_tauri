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
  try {
    const requested = new URLSearchParams(window.location.search).get("skin");
    if (isSkin(requested)) {
      window.localStorage.setItem(skinStorageKey, requested);
      return requested;
    }
    const stored = window.localStorage.getItem(skinStorageKey);
    return isSkin(stored) ? stored : "classic";
  } catch {
    return "classic";
  }
}

export function applySkinPreference(skin: SkinPreference) {
  document.documentElement.dataset.skin = skin;
  try {
    window.localStorage.setItem(skinStorageKey, skin);
  } catch {
    // The selected skin still applies for this session when storage is unavailable.
  }
}
