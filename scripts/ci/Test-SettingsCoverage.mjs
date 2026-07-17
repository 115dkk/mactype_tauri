import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");

function listProductionRustSources(directory) {
  const sources = [];
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const entryPath = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      if (entry.name !== "tests") sources.push(...listProductionRustSources(entryPath));
    } else if (entry.isFile() && entry.name.endsWith(".rs") && entry.name !== "tests.rs") {
      sources.push(entryPath);
    }
  }
  return sources.sort((left, right) => (left < right ? -1 : left > right ? 1 : 0));
}

const schema = JSON.parse(fs.readFileSync(path.join(root, "shared/settings-schema.json"), "utf8"));
const pairs = new Set(schema.map((setting) => `${setting.section}/${setting.key}`));
const required = [
  ["General", "HookChildProcesses"], ["General", "UseMapping"], ["General", "UseInclude"], ["General", "FontSubstitutes"],
  ["General", "CacheMaxFaces"], ["General", "CacheMaxSizes"], ["General", "CacheMaxBytes"],
  ["DirectWrite", "GammaValue"], ["DirectWrite", "Contrast"], ["DirectWrite", "RenderingMode"], ["DirectWrite", "ClearTypeLevel"],
  ["Experimental", "ClipBoxFix"], ["Experimental", "ColorFont"], ["Experimental", "InvertColor"],
  ...[
    "INFINALITY_FT_CHROMEOS_STYLE_SHARPENING_STRENGTH", "INFINALITY_FT_CONTRAST", "INFINALITY_FT_STEM_FITTING_STRENGTH",
    "INFINALITY_FT_AUTOHINT_SNAP_STEM_HEIGHT", "INFINALITY_FT_GRAYSCALE_FILTER_STRENGTH", "INFINALITY_FT_WINDOWS_STYLE_SHARPENING_STRENGTH",
    "INFINALITY_FT_BRIGHTNESS", "INFINALITY_FT_AUTOHINT_HORIZONTAL_STEM_DARKEN_STRENGTH", "INFINALITY_FT_STEM_ALIGNMENT_STRENGTH",
    "INFINALITY_FT_AUTOHINT_VERTICAL_STEM_DARKEN_STRENGTH", "INFINALITY_FT_FRINGE_FILTER_STRENGTH", "INFINALITY_FT_GLOBAL_EMBOLDEN_X_VALUE",
    "INFINALITY_FT_GLOBAL_EMBOLDEN_Y_VALUE", "INFINALITY_FT_BOLD_EMBOLDEN_X_VALUE", "INFINALITY_FT_BOLD_EMBOLDEN_Y_VALUE",
    "INFINALITY_FT_STEM_SNAPPING_SLIDING_SCALE", "INFINALITY_FT_USE_KNOWN_SETTINGS_ON_SELECTED_FONTS", "INFINALITY_FT_AUTOFIT_ADJUST_HEIGHTS",
    "INFINALITY_FT_USE_VARIOUS_TWEAKS", "INFINALITY_FT_AUTOHINT_INCREASE_GLYPH_HEIGHTS", "INFINALITY_FT_STEM_DARKENING_CFF", "INFINALITY_FT_STEM_DARKENING_AUTOFIT",
  ].map((key) => ["Infinality", key]),
];
const missing = required.filter(([section, key]) => !pairs.has(`${section}/${key}`));
if (missing.length) throw new Error(`Settings schema is missing core settings: ${missing.map((pair) => pair.join("/")).join(", ")}`);
if (schema.length !== 60) throw new Error(`Expected 60 scalar settings, found ${schema.length}`);

const rustRoot = path.join(root, "control-center/src-tauri/src");
const profileSources = [
  path.join(rustRoot, "profile.rs"),
  ...listProductionRustSources(path.join(rustRoot, "profile")),
];
const profile = profileSources.map((source) => fs.readFileSync(source, "utf8")).join("\n");
for (const key of ["Shadow", "LcdFilterWeight", "PixelLayout", "DisplayAffinity", "FontSubstitutes", "UnloadDLL", "ExcludeSub", "INFINALITY_FT_GAMMA_CORRECTION", "INFINALITY_FT_FILTER_PARAMS"]) {
  if (!profile.includes(`\"${key}\"`)) throw new Error(`Structured profile editor is missing ${key}`);
}

const settingsHeader = fs.readFileSync(path.join(root, "settings.h"), "utf8");
const shadowOffset = settingsHeader.match(/case ATTR_ShadowOffset:([\s\S]*?)case ATTR_Fontlink:/)?.[1] ?? "";
if (!/\bbreak\s*;/.test(shadowOffset)) throw new Error("ATTR_ShadowOffset still falls through into ATTR_Fontlink");
for (const attribute of ["ATTR_HookChildProcess", "ATTR_FontSubstitute", "ATTR_DirectWrite", "ATTR_PixelLayout"]) {
  if ((settingsHeader.match(new RegExp(`case ${attribute}:`, "g")) ?? []).length < 2) throw new Error(`${attribute} must support both SetIntAttribute and GetIntAttribute`);
}
console.log("Settings coverage gate passed for 60 scalar settings, structured INI settings, and IControlCenter fallthrough guards.");
