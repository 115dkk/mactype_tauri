import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./app/App";
import { loadSkinPreference } from "./app/skinPreference";
import { loadThemePreference } from "./app/themePreference";
import { I18nProvider } from "./i18n/I18nProvider";
import "./styles/tokens.css";
import "./styles/app.css";
import "./styles/skins/fluent.css";
import "./styles/skins/console.css";
import "./styles/skins/cupertino.css";

const root = document.getElementById("root");
const initialTheme = loadThemePreference();
const initialSkin = loadSkinPreference();

document.documentElement.dataset.theme = initialTheme;
document.documentElement.dataset.skin = initialSkin;

if (!root) {
  throw new Error("Control Center root element is missing");
}

createRoot(root).render(
  <StrictMode>
    <I18nProvider><App initialSkin={initialSkin} initialTheme={initialTheme} /></I18nProvider>
  </StrictMode>,
);
