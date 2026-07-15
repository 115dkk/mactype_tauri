export type ThemePreference = "light" | "dark";

export const themeStorageKey = "mactype-control-center.theme";

export function loadThemePreference(): ThemePreference {
  try {
    const stored = window.localStorage.getItem(themeStorageKey);
    return stored === "dark" || stored === "light" ? stored : "light";
  } catch {
    return "light";
  }
}

export function applyThemePreference(theme: ThemePreference) {
  document.documentElement.dataset.theme = theme;
  try {
    window.localStorage.setItem(themeStorageKey, theme);
  } catch {
    // The selected theme still applies for this session when storage is unavailable.
  }
}
