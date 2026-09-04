import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./app/App";
import { AppErrorBoundary } from "./app/AppErrorBoundary";
import { PreviewStudioApp } from "./studio/PreviewStudioApp";
import { loadSkinPreference } from "./app/skinPreference";
import { loadThemePreference } from "./app/themePreference";
import { I18nProvider } from "./i18n/I18nProvider";
import "./styles/tokens.css";
import "./styles/app.css";
import "./styles/skins/fluent.css";
import "./styles/skins/console.css";
import "./styles/skins/cupertino.css";
import "./styles/studio.css";

const root = document.getElementById("root");
const initialTheme = loadThemePreference();
const initialSkin = loadSkinPreference();

document.documentElement.dataset.theme = initialTheme;
document.documentElement.dataset.skin = initialSkin;

if (!root) {
  throw new Error("Control Center root element is missing");
}

/* The Preview Studio is the same bundle opened in a second window; the
   window label travels in the query so the shell can pick the right root. */
const isStudio = new URLSearchParams(window.location.search).get("window") === "preview-studio";
document.documentElement.dataset.window = isStudio ? "preview-studio" : "main";

createRoot(root).render(
  <StrictMode>
    <AppErrorBoundary><I18nProvider>{isStudio ? <PreviewStudioApp /> : <App initialSkin={initialSkin} initialTheme={initialTheme} />}</I18nProvider></AppErrorBoundary>
  </StrictMode>,
);
