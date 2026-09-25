#!/usr/bin/env node
// Builds this branch's Claude Design system files from the Control Center sources.
//
//   node build.mjs files --out <dir> --lucide <extracted lucide-static package>
//   node build.mjs index --out <dir> --blobs <blobs.json> [--existing <design-system.json>]
//
// `files` writes <dir>/project/** (everything except the index) and <dir>/uploads.json, the
// assets that must be uploaded to the system's asset store. `index` writes
// <dir>/project/design-system.json from the upload results; with --existing it keeps every key
// a person or an earlier sync set and only updates lastChange and the asset records.
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const skillDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(skillDir, "../../..");
const contentDir = path.join(skillDir, "content");
const target = readJson(path.join(skillDir, "target.json"));
const repoSlug = "115dkk/mactype_tauri";

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, "utf8"));
}

function readRepo(rel) {
  return fs.readFileSync(path.join(repoRoot, rel), "utf8");
}

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

function writeFile(file, data) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, data);
}

function parseArgs(argv) {
  const [command, ...rest] = argv;
  const options = {};
  for (let i = 0; i < rest.length; i += 2) {
    if (!rest[i].startsWith("--") || rest[i + 1] === undefined) fail(`bad argument ${rest[i]}`);
    options[rest[i].slice(2)] = rest[i + 1];
  }
  return { command, options };
}

function gitRevision() {
  const sha = execFileSync("git", ["-C", repoRoot, "rev-parse", "--short=7", "HEAD"], { encoding: "utf8" }).trim();
  return `${target.branch}@${sha}`;
}

// ---- CSS scanning -------------------------------------------------------------------------

// Index just past the comment, string or plain character at `i`.
function skipAtom(css, i) {
  if (css.startsWith("/*", i)) {
    const end = css.indexOf("*/", i + 2);
    return end < 0 ? css.length : end + 2;
  }
  const quote = css[i];
  if (quote === '"' || quote === "'") {
    let j = i + 1;
    while (j < css.length && css[j] !== quote) j += css[j] === "\\" ? 2 : 1;
    return j + 1;
  }
  return i + 1;
}

function matchingBrace(css, open) {
  let depth = 0;
  for (let i = open; i < css.length;) {
    if (css[i] === "{") depth += 1;
    else if (css[i] === "}") {
      depth -= 1;
      if (depth === 0) return i;
    }
    i = css[i] === "{" || css[i] === "}" ? i + 1 : skipAtom(css, i);
  }
  fail("unbalanced braces in stylesheet");
  return -1;
}

// Calls visit(prelude, body) for every top-level block and joins what it returns; `null` drops
// the block. Statements without a block (@import) and trailing text pass through unchanged.
function mapBlocks(css, visit) {
  let out = "";
  let start = 0;
  for (let i = 0; i < css.length;) {
    if (css[i] === "{") {
      const close = matchingBrace(css, i);
      const replaced = visit(css.slice(start, i), css.slice(i + 1, close));
      if (replaced !== null) out += replaced;
      start = close + 1;
      i = close + 1;
    } else if (css[i] === ";") {
      out += css.slice(start, i + 1);
      start = i + 1;
      i += 1;
    } else {
      i = skipAtom(css, i);
    }
  }
  return out + css.slice(start);
}

// Separates the comments and whitespace in front of a selector from the selector itself.
function splitLeading(prelude) {
  let i = 0;
  while (i < prelude.length) {
    if (/\s/.test(prelude[i])) i += 1;
    else if (prelude.startsWith("/*", i)) i = skipAtom(prelude, i);
    else break;
  }
  return [prelude.slice(0, i), prelude.slice(i).trim()];
}

function splitDeclarations(body) {
  const parts = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < body.length;) {
    const c = body[i];
    if (c === "(") depth += 1;
    else if (c === ")") depth -= 1;
    else if (c === ";" && depth === 0) {
      parts.push(body.slice(start, i));
      start = i + 1;
    }
    i = c === "(" || c === ")" || c === ";" ? i + 1 : skipAtom(body, i);
  }
  parts.push(body.slice(start));
  return parts.filter((part) => splitLeading(part)[1] !== "" || part.includes("/*"));
}

function customProperty(declaration) {
  const [, text] = splitLeading(declaration);
  const match = /^--([A-Za-z0-9_-]+)\s*:\s*([\s\S]*)$/.exec(text);
  return match ? { name: match[1], value: match[2].trim() } : null;
}

// A root-level block that declares tokens: :root, a theme, a skin, or a skin in a theme.
function tokenBlock(selector) {
  let match = /^:root(?:\[data-theme="(light|dark)"\])?$/.exec(selector);
  if (match) return { skin: null, mode: match[1] ?? null, specificity: match[1] ? 20 : 10 };
  match = /^html\[data-skin="([a-z]+)"\](?:\[data-theme="(light|dark)"\])?$/.exec(selector);
  if (match) return { skin: match[1], mode: match[2] ?? null, specificity: match[2] ? 21 : 11 };
  return null;
}

// Stylesheets in the order control-center/src/main.tsx imports them.
function stylesheets() {
  const main = readRepo("control-center/src/main.tsx");
  const files = [...main.matchAll(/^import\s+"\.\/(styles\/[^"]+\.css)";/gm)].map((m) => `control-center/src/${m[1]}`);
  if (files.length === 0) fail("no stylesheet imports found in control-center/src/main.tsx");
  return files.map((file) => ({ file, css: readRepo(file) }));
}

// ---- Tokens -------------------------------------------------------------------------------

const colorLiteral = /^(#(?:[0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})|(?:rgba?|hsla?)\(\s*[0-9.,%\s/+-]+\))$/i;
const lengthLiteral = /^-?\d+(?:\.\d+)?(?:px|rem|em|%)$|^0$/;

function collectDeclarations(sheets) {
  const declarations = [];
  let order = 0;
  for (const { file, css } of sheets) {
    mapBlocks(css, (prelude, body) => {
      const block = tokenBlock(splitLeading(prelude)[1]);
      if (block) {
        for (const part of splitDeclarations(body)) {
          const property = customProperty(part);
          if (property) declarations.push({ ...block, ...property, file, order: order++ });
        }
      }
      return "";
    });
  }
  return declarations;
}

function themesFor(declarations) {
  const skins = [...new Set(declarations.filter((d) => d.skin).map((d) => d.skin))];
  if (skins.length === 0) {
    return [
      { id: "light", name: "Light", skin: null, mode: "light" },
      { id: "dark", name: "Dark", skin: null, mode: "dark" },
    ];
  }
  const title = (word) => word[0].toUpperCase() + word.slice(1);
  return ["classic", ...skins].flatMap((skin) => ["light", "dark"].map((mode) => ({
    id: `${skin}-${mode}`,
    name: `${title(skin)} ${mode}`,
    skin: skin === "classic" ? null : skin,
    mode,
  })));
}

// The value the Control Center computes for `name` under a theme: highest specificity wins,
// then the later declaration.
function resolve(declarations, theme, name) {
  let winner = null;
  for (const d of declarations) {
    if (d.name !== name) continue;
    if (d.skin !== null && d.skin !== theme.skin) continue;
    if (d.mode !== null && d.mode !== theme.mode) continue;
    if (!winner || d.specificity > winner.specificity || (d.specificity === winner.specificity && d.order > winner.order)) winner = d;
  }
  return winner?.value;
}

function familyFor(name, value) {
  if (/^space-/.test(name)) return "spacing";
  if (/^radius-/.test(name)) return "radius";
  if (/^shadow-/.test(name)) return "shadow";
  if (/^opacity-/.test(name)) return "opacity";
  if (/^(motion|ease)-/.test(name)) return null;
  return lengthLiteral.test(value) ? "size" : null;
}

function rgba(value) {
  const hex = /^#([0-9a-f]+)$/i.exec(value);
  if (hex) {
    const h = hex[1].length <= 4 ? [...hex[1]].map((c) => c + c).join("") : hex[1];
    return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16)).concat(h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1);
  }
  const parts = /\(([^)]*)\)/.exec(value)[1].split(/[\s,/]+/).filter(Boolean).map(Number);
  return [parts[0], parts[1], parts[2], parts[3] ?? 1];
}

function contrast(foreground, ground) {
  const over = (top, bottom) => top.slice(0, 3).map((c, i) => c * top[3] + bottom[i] * (1 - top[3]));
  const bg = over(rgba(ground), [255, 255, 255]);
  const fg = over(rgba(foreground), bg);
  const luminance = (rgb) => rgb.map((c) => {
    const s = c / 255;
    return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  }).reduce((sum, c, i) => sum + c * [0.2126, 0.7152, 0.0722][i], 0);
  const [a, b] = [luminance(fg), luminance(bg)].sort((x, y) => y - x);
  return (a + 0.05) / (b + 0.05);
}

// Appends every failing pair from content/contrast.json to the foreground token's note, so a
// source colour that misses WCAG stays exact and says so.
function flagContrast(colorTokens, themes) {
  const pairs = readJson(path.join(contentDir, "contrast.json"));
  const byName = new Map(colorTokens.map((t) => [t.name, t]));
  for (const { foreground, grounds, minimum } of pairs) {
    const token = byName.get(foreground);
    if (!token) continue;
    const misses = [];
    for (const theme of themes) {
      for (const ground of grounds) {
        const bg = byName.get(ground);
        const fgValue = token.value[theme.id];
        const bgValue = bg?.value[theme.id];
        if (!fgValue || !bgValue) continue;
        const ratio = contrast(fgValue, bgValue);
        if (ratio < minimum) misses.push(`${ground} in ${theme.id} (${ratio.toFixed(2)}:1)`);
      }
    }
    if (misses.length) token.usage += ` Source value kept: under ${minimum}:1 on ${misses.join(", ")}.`;
  }
}

function buildTokens(declarations, notes) {
  const themes = themesFor(declarations);
  const names = [...new Set(declarations.map((d) => d.name))];
  const missing = [];
  const note = (name) => {
    if (!notes[name]) missing.push(name);
    return notes[name] ?? "";
  };
  const colorTokens = [];
  const families = { spacing: [], radius: [], shadow: [], size: [], opacity: [] };
  const tokenized = { color: new Set(), base: new Set() };
  for (const name of names) {
    const all = declarations.filter((d) => d.name === name);
    if (all.every((d) => colorLiteral.test(d.value))) {
      const value = {};
      for (const theme of themes) {
        const resolved = resolve(declarations, theme, name);
        if (resolved !== undefined) value[theme.id] = resolved.replace(/^#[0-9A-F]+$/i, (hex) => hex.toLowerCase());
      }
      colorTokens.push({ name, value, usage: note(name) });
      tokenized.color.add(name);
      continue;
    }
    const base = all.find((d) => d.skin === null && d.mode === null);
    if (!base) continue;
    const family = familyFor(name, base.value);
    if (!family) continue;
    families[family].push({ name, value: base.value, usage: note(name) });
    tokenized.base.add(name);
  }
  if (missing.length) fail(`add a usage note to content/token-notes.json for: ${missing.join(", ")}`);
  flagContrast(colorTokens, themes);
  const type = readJson(path.join(contentDir, "type.json"));
  const tokens = {
    name: target.title,
    version: 1,
    meta: {
      source: "github",
      repo: repoSlug,
      ref: gitRevision(),
      package: "control-center",
      paths: {
        tokens: stylesheets().map((s) => s.file),
        fonts: type.fonts.map((f) => f.source),
        assets: ["control-center/public/mactype-icon.png", "lucide-react (control-center/package.json)"],
        docs: ["DESIGN.md", ...(target.sections ?? []).map((s) => s.from)],
      },
      synced: new Date().toISOString().slice(0, 10),
    },
    color: { themes: themes.map(({ id, name }) => ({ id, name })), tokens: colorTokens },
    type: {
      fonts: type.fonts.map(({ family, file, weight, style }) => ({ family, file, weight, style })),
      families: type.families,
      groups: type.groups,
    },
  };
  for (const [family, list] of Object.entries(families)) {
    if (list.length) tokens[family] = { ...(type.notes?.[family] ? { note: type.notes[family] } : {}), tokens: list };
  }
  return { tokens, tokenized, themes };
}

// ---- bundle.css ---------------------------------------------------------------------------

// Skin rules match the preview frame's theme id ("fluent-dark") instead of the app's separate
// data-skin and data-theme attributes. `div` lets a preview pin one skin on a wrapper element
// while keeping each selector's specificity unchanged.
function rewriteSelector(selector) {
  return selector
    .replace(/(^|[\s,>+~(])html(?=((?:\[[^\]]*\])+))/g, (match, lead, attributes) => (attributes.includes("data-skin") ? `${lead}:is(html, div)` : match))
    .replace(/\[data-skin="([a-z]+)"\]/g, '[data-theme^="$1-"]')
    .replace(/\[data-skin\]/g, "[data-theme]")
    .replace(/\[data-theme="(light|dark)"\]/g, '[data-theme$="-$1"]');
}

function bundleCss(sheets, tokenized, skinned) {
  const transform = (css, nested) => mapBlocks(css, (prelude, body) => {
    const [leading, selector] = splitLeading(prelude);
    if (/^@font-face$/i.test(selector)) return null;
    if (/^@(media|supports|container|layer)\b/i.test(selector)) return `${prelude}{${transform(body, true)}}`;
    if (selector.startsWith("@")) return `${prelude}{${body}}`;
    const block = nested ? null : tokenBlock(selector);
    let kept = body;
    if (block) {
      const parts = splitDeclarations(body).filter((part) => {
        const property = customProperty(part);
        if (!property) return true;
        return !(tokenized.color.has(property.name) || (block.skin === null && block.mode === null && tokenized.base.has(property.name)));
      });
      if (parts.every((part) => splitLeading(part)[1] === "")) return null;
      kept = `${parts.map((part) => `\n  ${part.trim()};`).join("")}\n`;
    }
    const nextSelector = skinned ? rewriteSelector(selector) : selector;
    return `${leading}${nextSelector} {${kept}}`;
  });
  const header = [
    `/* ${target.title}: the Control Center stylesheets at ${gitRevision()}, in import order:`,
    ...sheets.map((s) => `   ${s.file}`),
    "   Token declarations and @font-face rules live in tokens.css, which loads first.",
    ...(skinned ? ["   Skin selectors key on the theme id (classic-light ... cupertino-dark); a",
      "   <div data-theme=\"<skin>-<mode>\"> wrapper pins one skin inside a preview. */"] : ["*/"]),
  ].join("\n");
  const body = sheets.map((s) => `\n/* ---- ${s.file} ---- */\n${transform(s.css, false).trim()}\n`).join("");
  const css = `${header}\n${body}`;
  if (/<\/style/i.test(css)) fail("bundle.css contains </style");
  return css;
}

// ---- Icons --------------------------------------------------------------------------------

function lucideIndex(lucideDir) {
  const esm = fs.readFileSync(path.join(lucideDir, "dist/esm/lucide-static.js"), "utf8");
  const map = new Map();
  for (const m of esm.matchAll(/export \{([^}]*)\} from '\.\/icons\/([a-z0-9-]+)\.js'/g)) {
    for (const alias of m[1].matchAll(/default as (\w+)/g)) map.set(alias[1], m[2]);
  }
  return map;
}

function usedIcons() {
  const names = new Set();
  const walk = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) walk(full);
      else if (/\.tsx?$/.test(entry.name)) {
        for (const m of fs.readFileSync(full, "utf8").matchAll(/import\s*\{([^}]+)\}\s*from\s*"lucide-react"/g)) {
          for (const part of m[1].split(",")) {
            const name = part.trim().replace(/^type\s+/, "");
            if (/^[A-Z]\w*$/.test(name) && name !== "LucideIcon") names.add(name);
          }
        }
      }
    }
  };
  walk(path.join(repoRoot, "control-center/src"));
  return [...names].sort();
}

function inlineIcon(lucideDir, index, name, size, stroke) {
  const file = index.get(name);
  if (!file) fail(`unknown lucide icon ${name}`);
  const svg = fs.readFileSync(path.join(lucideDir, "icons", `${file}.svg`), "utf8")
    .replace(/<!--[\s\S]*?-->\s*/, "")
    .replace(/\s*\n\s*/g, " ")
    .replace(/ class="[^"]*"/, "")
    .replace(/ width="24"/, ` width="${size}"`)
    .replace(/ height="24"/, ` height="${size}"`)
    .replace(/ stroke-width="2"/, ` stroke-width="${stroke}"`)
    .replace("<svg ", '<svg aria-hidden="true" ')
    .trim();
  return svg;
}

// ---- files --------------------------------------------------------------------------------

function copyTree(fromDir, toDir, transform) {
  if (!fs.existsSync(fromDir)) return [];
  const written = [];
  for (const entry of fs.readdirSync(fromDir, { withFileTypes: true })) {
    const from = path.join(fromDir, entry.name);
    const to = path.join(toDir, entry.name);
    if (entry.isDirectory()) written.push(...copyTree(from, to, transform));
    else {
      writeFile(to, transform ? transform(from, fs.readFileSync(from, "utf8")) : fs.readFileSync(from));
      written.push(to);
    }
  }
  return written;
}

// Intent classes the markup carries for readability while the base rule draws them
// (`button secondary` is the plain `.button`).
const markerClasses = new Set(fs.existsSync(path.join(contentDir, "marker-classes.json")) ? readJson(path.join(contentDir, "marker-classes.json")) : []);

function checkPreviewClasses(file, html, css) {
  const missing = new Set();
  for (const m of html.matchAll(/class="([^"]+)"/g)) {
    for (const name of m[1].split(/\s+/)) {
      if (!name || name.startsWith("ds-") || markerClasses.has(name)) continue;
      const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      if (!new RegExp(`\\.${escaped}(?![\\w-])`).test(css)) missing.add(name);
    }
  }
  if (missing.size) console.warn(`warning: ${path.relative(skillDir, file)} uses classes the stylesheets never style: ${[...missing].join(", ")}`);
}

function buildFiles(options) {
  if (!options.out) fail("--out is required");
  if (!options.lucide) fail("--lucide is required (extract `npm pack lucide-static@<control-center lucide-react version>`)");
  const out = path.resolve(options.out);
  const project = path.join(out, "project");
  fs.rmSync(out, { recursive: true, force: true });

  const sheets = stylesheets();
  const declarations = collectDeclarations(sheets);
  const notes = readJson(path.join(contentDir, "token-notes.json"));
  const { tokens, tokenized, themes } = buildTokens(declarations, notes);
  const skinned = themes.some((theme) => theme.skin);
  writeFile(path.join(project, "tokens.json"), `${JSON.stringify(tokens, null, 2)}\n`);
  const css = bundleCss(sheets, tokenized, skinned);
  writeFile(path.join(project, "components/bundle.css"), css);

  const type = readJson(path.join(contentDir, "type.json"));
  for (const font of [...type.fonts, ...(type.files ?? [])]) {
    writeFile(path.join(project, font.file), fs.readFileSync(path.join(repoRoot, font.source)));
  }

  writeFile(path.join(project, "README.md"), fs.readFileSync(path.join(contentDir, "README.md")));
  for (const section of target.sections ?? []) {
    writeFile(path.join(project, section.to), readRepo(section.from));
  }

  const lucideDir = path.resolve(options.lucide);
  const index = lucideIndex(lucideDir);
  const logo = fs.readFileSync(path.join(repoRoot, "control-center/public/mactype-icon.png"));
  const logoUri = `data:image/png;base64,${logo.toString("base64")}`;
  const expand = (file, text) => {
    if (!file.endsWith(".html")) return text;
    const html = text
      .replaceAll("{{logo}}", logoUri)
      .replace(/\{\{icon:(\w+):(\d+)(?::([\d.]+))?\}\}/g, (_, name, size, stroke) => inlineIcon(lucideDir, index, name, size, stroke ?? "2"));
    checkPreviewClasses(file, html, css);
    if (/<\/?(iframe|frame|object|embed|portal|noscript)\b/i.test(html)) fail(`${file} uses an element previews refuse`);
    return html;
  };
  copyTree(path.join(contentDir, "components"), path.join(project, "components"), expand);
  copyTree(path.join(contentDir, "assets"), path.join(project, "assets"));

  const uploads = [];
  const logoPath = path.join(out, "upload/Logos/mactype-icon.png");
  writeFile(logoPath, logo);
  uploads.push({ group: "Logos", name: "mactype-icon.png", localPath: logoPath, size: logo.length, type: "image/png" });
  const icons = usedIcons();
  const iconRows = [];
  for (const name of icons) {
    const file = index.get(name);
    if (!file) fail(`lucide-static has no icon for lucide-react export ${name}`);
    const svg = fs.readFileSync(path.join(lucideDir, "icons", `${file}.svg`));
    const localPath = path.join(out, `upload/Icons/${file}.svg`);
    if (!fs.existsSync(localPath)) {
      writeFile(localPath, svg);
      uploads.push({ group: "Icons", name: `${file}.svg`, localPath, size: svg.length, type: "image/svg+xml" });
    }
    iconRows.push(`| \`${name}\` | \`${file}.svg\` |`);
  }
  const iconReadme = path.join(project, "assets/Icons/README.md");
  writeFile(iconReadme, `${fs.readFileSync(iconReadme, "utf8").trimEnd()}\n\n| lucide-react export | File |\n| --- | --- |\n${iconRows.join("\n")}\n`);
  writeFile(path.join(out, "uploads.json"), `${JSON.stringify(uploads, null, 2)}\n`);

  const files = [];
  const list = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) list(full);
      else files.push(path.relative(out, full).split(path.sep).join("/"));
    }
  };
  list(project);
  writeFile(path.join(out, "files.json"), `${JSON.stringify(Object.fromEntries(files.map((f) => [f, f])), null, 2)}\n`);
  console.log(`${files.length} project files, ${uploads.length} uploads, ${themes.length} themes, ${tokens.color.tokens.length} colors -> ${out}`);
}

// ---- index --------------------------------------------------------------------------------

function assetKey(name) {
  return [...Buffer.from(name, "utf8")].map((byte) => {
    const c = String.fromCharCode(byte);
    return /[A-Za-z0-9_./-]/.test(c) ? c : `~${byte.toString(16).padStart(2, "0")}`;
  }).join("");
}

function buildIndex(options) {
  if (!options.out || !options.blobs) fail("--out and --blobs are required");
  const out = path.resolve(options.out);
  const uploads = readJson(path.join(out, "uploads.json"));
  const blobs = readJson(path.resolve(options.blobs));
  const now = new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
  const existing = options.existing ? readJson(path.resolve(options.existing)) : null;
  if (existing && !existing.createdOnFiles && !existing.convertedFrom) fail("the existing index has no createdOnFiles/convertedFrom marker");
  const index = existing ?? {
    v: 3,
    layout: "files",
    createdOnFiles: { v: 1, at: now },
    title: target.title,
    namespace: target.namespace,
    libraries: [],
    sections: {},
    groups: ["Logos", "Icons"],
    assetGroups: {},
    blobs: {},
    docs: { readme: "project/README.md", sections: [] },
  };
  const tiles = { Logos: "l", Icons: "s" };
  for (const group of ["Logos", "Icons"]) {
    index.assetGroups[group] ??= { name: group, tile: tiles[group], order: [], files: {} };
    if (!index.groups.includes(group)) index.groups.push(group);
  }
  for (const upload of uploads) {
    // The asset store may normalise a file (SVG comments and whitespace), so the size it
    // reports wins over the local one.
    const entry = blobs[`${upload.group}/${upload.name}`];
    if (!entry) {
      if (existing?.assetGroups?.[upload.group]?.files?.[assetKey(upload.name)]) continue;
      fail(`no blob id for ${upload.group}/${upload.name}`);
    }
    const { id, size } = typeof entry === "string" ? { id: entry, size: upload.size } : entry;
    const group = index.assetGroups[upload.group];
    group.files[assetKey(upload.name)] = { name: upload.name, blob: id.replace(/^\/_blob\//, ""), size, type: upload.type };
    if (!group.order.includes(upload.name)) group.order.push(upload.name);
  }
  index.lastChange = {
    by: options.by ?? "Claude",
    at: now,
    via: `GitHub · ${repoSlug}@${gitRevision().split("@")[1]}`,
    note: options.note ?? `Synced from ${gitRevision()}`,
  };
  writeFile(path.join(out, "project/design-system.json"), `${JSON.stringify(index, null, 2)}\n`);
  console.log(`index -> ${path.join(out, "project/design-system.json")}`);
}

const { command, options } = parseArgs(process.argv.slice(2));
if (command === "files") buildFiles(options);
else if (command === "index") buildIndex(options);
else fail("usage: build.mjs files|index --out <dir> ...");
