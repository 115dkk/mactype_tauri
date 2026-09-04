import { AppWindow, Contrast, Download, RotateCcw, Search } from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { ProfileEntry } from "../app/model";
import { listProfiles, loadInstalledFontFamilies, pickPngExportPath, setNativePreview, writePreviewExport } from "../app/tauri";
import { useAppTheme } from "../app/useAppTheme";
import { Segmented } from "../components/Segmented";
import { SwitchControl } from "../components/SwitchControl";
import { WindowTitleBar } from "../components/WindowTitleBar";
import { currentSkin, nativeChrome } from "../features/preview/nativeChrome";
import { nativePreviewLabels } from "../features/preview/nativePreviewLabels";
import { useSpecimenRenders } from "../features/preview/useSpecimenRenders";
import { useI18n } from "../i18n/i18n";
import { requestStudioDocument, subscribeStudioDocument } from "./studioBridge";
import { StudioBoard, StudioLoupe, type LoupeState } from "./StudioBoard";
import { composeStudioPng } from "./studioExport";
import {
  compareLabelKeys,
  loadStudioSettings,
  MAX_STUDIO_FONTS,
  presetLabelKeys,
  presetTexts,
  resolveSource,
  saveStudioSettings,
  sourceKindLabelKeys,
  STUDIO_DPIS,
  STUDIO_SIZE_LADDER,
  STUDIO_ZOOMS,
  nativePalette,
  studioPalette,
  studioRequests,
  studioText,
  styleLabelKeys,
  type StudioCompare,
  type StudioDocument,
  type StudioDpi,
  type StudioPreset,
  type StudioSettings,
  type StudioSource,
  type StudioSourceKind,
  type StudioStyle,
  type StudioZoom,
} from "./studioModel";

const BLINK_INTERVAL_MS = 700;

/* The Preview Studio: a second window that renders the same sample through
   the helper for several fonts, sizes and styles, and compares two sources
   (the Tuner's edits, the saved profile, any profile file, or Windows' own
   rendering) side by side, stacked, blinking, or as a pixel difference, at
   integer zoom with a loupe. Nothing here scales a bitmap with CSS. */
export function PreviewStudioApp() {
  const { locale, t } = useI18n();
  const theme = useAppTheme();
  const [settings, setSettings] = useState<StudioSettings>(loadStudioSettings);
  const [document, setDocument] = useState<StudioDocument | null>(null);
  const [profiles, setProfiles] = useState<ReadonlyArray<ProfileEntry>>([]);
  const [fonts, setFonts] = useState<ReadonlyArray<string>>([]);
  const [fontQuery, setFontQuery] = useState("");
  const [boardWidth, setBoardWidth] = useState(0);
  const [blinkB, setBlinkB] = useState(false);
  const [loupe, setLoupe] = useState<LoupeState | null>(null);
  const [exportMessage, setExportMessage] = useState<string | null>(null);
  const [exporting, setExporting] = useState(false);
  const boardsRef = useRef<HTMLDivElement>(null);

  const update = useCallback((patch: Partial<StudioSettings>) => setSettings((current) => ({ ...current, ...patch })), []);
  useEffect(() => saveStudioSettings(settings), [settings]);

  useEffect(() => {
    let active = true;
    void listProfiles().then((entries) => {
      if (active) setProfiles(entries);
    }).catch(() => undefined);
    void loadInstalledFontFamilies().then((families) => {
      if (active) setFonts(families);
    }).catch(() => undefined);
    return () => { active = false; };
  }, []);

  /* The Tuner document arrives from the main window; ask once on open and
     keep following while the switch is on. */
  useEffect(() => {
    const stop = subscribeStudioDocument((next) => {
      setDocument((current) => (settings.follow || current === null ? next : current));
    });
    requestStudioDocument();
    return stop;
  }, [settings.follow]);

  useEffect(() => {
    const boards = boardsRef.current;
    if (!boards) return undefined;
    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) setBoardWidth(Math.floor(entry.contentRect.width / 8) * 8);
    });
    observer.observe(boards);
    return () => observer.disconnect();
  }, []);

  const twoBoards = settings.sourceB.kind !== "none";
  const showDiff = twoBoards && settings.compare === "diff";
  const sideBySide = twoBoards && settings.compare === "side";
  const gap = 16;
  const columnWidth = sideBySide ? Math.floor((boardWidth - gap) / 2) : boardWidth;
  const stripWidth = Math.max(0, Math.floor((columnWidth - 32) / settings.zoom));
  const fallbackProfile = profiles[0]?.path ?? null;
  const sourceA = useMemo(() => resolveSource(settings.sourceA, document, fallbackProfile), [document, fallbackProfile, settings.sourceA]);
  const sourceB = useMemo(() => resolveSource(settings.sourceB, document, fallbackProfile), [document, fallbackProfile, settings.sourceB]);
  const requestsA = useMemo(() => studioRequests(settings, sourceA, stripWidth, "A", theme), [settings, sourceA, stripWidth, theme]);
  const requestsB = useMemo(() => studioRequests(settings, sourceB, stripWidth, "B", theme), [settings, sourceB, stripWidth, theme]);
  const rendersA = useSpecimenRenders(requestsA);
  const rendersB = useSpecimenRenders(requestsB, twoBoards);
  const palette = studioPalette(settings, theme);

  useEffect(() => {
    if (!twoBoards || settings.compare !== "blink") return undefined;
    const timer = window.setInterval(() => setBlinkB((value) => !value), BLINK_INTERVAL_MS);
    return () => window.clearInterval(timer);
  }, [settings.compare, twoBoards]);

  useEffect(() => {
    const listener = (event: KeyboardEvent) => {
      const target = event.target;
      if (target instanceof HTMLInputElement || target instanceof HTMLTextAreaElement || target instanceof HTMLSelectElement) return;
      if (event.key === " " && twoBoards && settings.compare === "blink") {
        event.preventDefault();
        setBlinkB((value) => !value);
      } else if (event.key === "+" || event.key === "=") {
        const next = STUDIO_ZOOMS[Math.min(STUDIO_ZOOMS.length - 1, STUDIO_ZOOMS.indexOf(settings.zoom) + 1)];
        update({ zoom: next });
      } else if (event.key === "-") {
        const next = STUDIO_ZOOMS[Math.max(0, STUDIO_ZOOMS.indexOf(settings.zoom) - 1)];
        update({ zoom: next });
      }
    };
    window.addEventListener("keydown", listener);
    return () => window.removeEventListener("keydown", listener);
  }, [settings.compare, settings.zoom, twoBoards, update]);

  const sourceLabel = (source: StudioSource, resolved: ReturnType<typeof resolveSource>) => {
    const base = t(sourceKindLabelKeys[source.kind]);
    const profile = resolved.profilePath ? resolved.profilePath.split(/[\\/]/).pop() ?? null : null;
    return profile && source.kind !== "none" ? t("studio.sourceLabel", { label: base, profile }) : base;
  };
  const unavailableMessage = (resolved: ReturnType<typeof resolveSource>) => resolved.unavailable === "no-document" ? t("studio.noDocument") : resolved.unavailable === "no-profile" ? t("profiles.none") : null;

  const toggleFont = (font: string) => {
    const has = settings.fonts.includes(font);
    if (has && settings.fonts.length === 1) return;
    if (!has && settings.fonts.length >= MAX_STUDIO_FONTS) return;
    update({ fonts: has ? settings.fonts.filter((entry) => entry !== font) : [...settings.fonts, font] });
  };
  const toggleSize = (size: number) => {
    const has = settings.sizes.includes(size);
    if (has && settings.sizes.length === 1) return;
    update({ sizes: (has ? settings.sizes.filter((entry) => entry !== size) : [...settings.sizes, size]).sort((left, right) => left - right) });
  };
  const toggleStyle = (style: StudioStyle) => {
    const has = settings.styles.includes(style);
    if (has && settings.styles.length === 1) return;
    const order: StudioStyle[] = ["regular", "bold", "italic", "boldItalic"];
    update({ styles: order.filter((entry) => (entry === style ? !has : settings.styles.includes(entry))) });
  };
  const chooseSource = (side: "sourceA" | "sourceB", kind: StudioSourceKind) => {
    const current = settings[side];
    update({ [side]: { kind, profilePath: kind === "profile" || kind === "plain" ? current.profilePath ?? fallbackProfile : null } });
  };
  const chooseSourceProfile = (side: "sourceA" | "sourceB", path: string) => {
    update({ [side]: { ...settings[side], profilePath: path } });
  };

  const exportPng = async () => {
    setExporting(true);
    setExportMessage(null);
    try {
      const boards = [{ label: sourceLabel(settings.sourceA, sourceA), lines: rendersA.lines }];
      if (twoBoards) boards.push({ label: sourceLabel(settings.sourceB, sourceB), lines: rendersB.lines });
      const png = await composeStudioPng(boards, palette.background, palette.foreground);
      const path = await pickPngExportPath(t("studio.pngFilter"), `mactype-specimen-${new Date().toISOString().slice(0, 10)}.png`);
      if (!path) return;
      const written = await writePreviewExport(path, png);
      setExportMessage(t("studio.exported", { path: written }));
    } catch (caught: unknown) {
      setExportMessage(caught instanceof Error ? caught.message : String(caught));
    } finally {
      setExporting(false);
    }
  };
  const showNative = async () => {
    try {
      await setNativePreview(true, {
        displayMode: "sample",
        text: studioText(settings),
        listingText: studioText(settings).split("\n")[0],
        fontFace: settings.fonts[0],
        fontSizePt: settings.sizes[0],
        ...nativePalette(settings, theme),
        theme,
        sizes: settings.sizes,
        labels: nativePreviewLabels(t),
        chrome: nativeChrome(currentSkin(), theme),
      });
    } catch (caught: unknown) {
      setExportMessage(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const fontNeedle = fontQuery.trim().toLocaleLowerCase();
  const collator = useMemo(() => new Intl.Collator(locale, { sensitivity: "base", numeric: true }), [locale]);
  const fontList = useMemo(() => [...new Set([...settings.fonts, ...fonts])].sort((left, right) => collator.compare(left, right)).filter((font) => !fontNeedle || font.toLocaleLowerCase().includes(fontNeedle)), [collator, fontNeedle, fonts, settings.fonts]);
  const stripCount = rendersA.lines.length + (twoBoards ? rendersB.lines.length : 0);
  const boardsClass = `studio-boards studio-boards-${twoBoards ? (settings.compare === "diff" || settings.compare === "blink" ? "single" : settings.compare) : "single"}`;

  return (
    <div className="studio-app" data-testid="preview-studio">
      <WindowTitleBar title={t("studio.title")} />
      <div className="studio-frame">
        <aside className="studio-controls" aria-label={t("studio.title")}>
          <section className="studio-section">
            <h2>{t("studio.sources")}</h2>
            {(["sourceA", "sourceB"] as const).map((side) => {
              const source = settings[side];
              return (
                <div className="studio-source" key={side}>
                  <label className="studio-source-kind"><span className="studio-source-tag">{side === "sourceA" ? t("studio.sourceA") : t("studio.sourceB")}</span>
                    <select aria-label={side === "sourceA" ? t("studio.sourceA") : t("studio.sourceB")} onChange={(event) => chooseSource(side, event.target.value as StudioSourceKind)} value={source.kind}>
                      {(Object.keys(sourceKindLabelKeys) as StudioSourceKind[]).filter((kind) => side === "sourceB" || kind !== "none").map((kind) => <option key={kind} value={kind}>{t(sourceKindLabelKeys[kind])}</option>)}
                    </select>
                  </label>
                  {(source.kind === "profile" || source.kind === "plain") && (
                    <select aria-label={t("studio.source.profile")} className="studio-source-profile" onChange={(event) => chooseSourceProfile(side, event.target.value)} value={source.profilePath ?? fallbackProfile ?? ""}>
                      {profiles.map((entry) => <option key={entry.path} value={entry.path}>{entry.name}</option>)}
                    </select>
                  )}
                </div>
              );
            })}
            <Segmented compact label={t("studio.compare")} onChange={(value: StudioCompare) => update({ compare: value })} options={(Object.keys(compareLabelKeys) as StudioCompare[]).map((value) => ({ value, label: t(compareLabelKeys[value]) }))} value={settings.compare} />
            {twoBoards && settings.compare === "blink" && <p className="studio-hint">{t("studio.blinkHint")}</p>}
            {showDiff && <p className="studio-hint">{t("studio.diffHint")}</p>}
            <label className="studio-follow"><span>{t("studio.follow")}</span><SwitchControl checked={settings.follow} label={t("studio.follow")} onChange={(checked) => update({ follow: checked })} /></label>
            {!document && <p className="studio-hint studio-hint-warn">{t("studio.noDocument")}</p>}
          </section>

          <section className="studio-section">
            <h2>{t("studio.fonts")}<span>{settings.fonts.length} / {MAX_STUDIO_FONTS}</span></h2>
            <label className="studio-search search-field"><Search aria-hidden="true" size={14} /><span className="sr-only">{t("studio.fontSearch")}</span><input onChange={(event) => setFontQuery(event.target.value)} placeholder={t("studio.fontSearch")} type="search" value={fontQuery} /></label>
            <ul className="studio-font-list">
              {fontList.map((font) => {
                const checked = settings.fonts.includes(font);
                return <li key={font}><label className="studio-font" data-checked={checked}><input checked={checked} disabled={!checked && settings.fonts.length >= MAX_STUDIO_FONTS} onChange={() => toggleFont(font)} type="checkbox" /><span style={{ fontFamily: `"${font}", sans-serif` }}>{font}</span></label></li>;
              })}
            </ul>
            <p className="studio-hint">{t("studio.fontLimit", { count: MAX_STUDIO_FONTS })}</p>
          </section>

          <section className="studio-section">
            <h2>{t("studio.sizes")}</h2>
            <div className="studio-chips" role="group" aria-label={t("studio.sizes")}>
              {[...new Set([...STUDIO_SIZE_LADDER, ...settings.sizes])].sort((left, right) => left - right).map((size) => (
                <button aria-pressed={settings.sizes.includes(size)} className="studio-chip" key={size} onClick={() => toggleSize(size)} type="button">{size}</button>
              ))}
              <form className="studio-add-size" onSubmit={(event) => {
                event.preventDefault();
                const input = event.currentTarget.elements.namedItem("size") as HTMLInputElement | null;
                const value = Number(input?.value);
                if (input && Number.isFinite(value) && value >= 6 && value <= 96 && !settings.sizes.includes(value)) {
                  update({ sizes: [...settings.sizes, value].sort((left, right) => left - right) });
                  input.value = "";
                }
              }}><input aria-label={t("studio.customSize")} inputMode="numeric" max={96} min={6} name="size" placeholder="pt" type="number" /><button className="button secondary" type="submit">{t("studio.customSize")}</button></form>
            </div>
          </section>

          <section className="studio-section">
            <h2>{t("studio.styles")}</h2>
            <div className="studio-chips" role="group" aria-label={t("studio.styles")}>
              {(Object.keys(styleLabelKeys) as StudioStyle[]).map((style) => <button aria-pressed={settings.styles.includes(style)} className="studio-chip" data-style={style} key={style} onClick={() => toggleStyle(style)} type="button">{t(styleLabelKeys[style])}</button>)}
            </div>
          </section>

          <section className="studio-section">
            <h2>{t("studio.text")}</h2>
            <select aria-label={t("studio.text")} onChange={(event) => {
              const preset = event.target.value as StudioPreset;
              update(preset === "custom" ? { preset, customText: settings.customText || studioText(settings) } : { preset, customText: presetTexts[preset] });
            }} value={settings.preset}>
              {(Object.keys(presetLabelKeys) as StudioPreset[]).map((preset) => <option key={preset} value={preset}>{t(presetLabelKeys[preset])}</option>)}
            </select>
            <textarea aria-label={t("studio.text")} className="sample-input studio-text" onChange={(event) => update({ preset: "custom", customText: event.target.value })} rows={3} value={studioText(settings)} />
          </section>

          <section className="studio-section">
            <h2>{t("studio.palette")}</h2>
            <div className="studio-palette-row">
              <Segmented compact label={t("studio.palette")} onChange={(value: "theme" | "custom") => update({ palette: value })} options={[{ value: "theme", label: t("studio.palette.auto") }, { value: "custom", label: t("studio.palette.custom") }]} value={settings.palette} />
              <button aria-pressed={settings.inverted} className="button ghost" disabled={settings.palette === "custom"} onClick={() => update({ inverted: !settings.inverted })} type="button"><Contrast aria-hidden="true" size={14} /> {t("profiles.invertColours")}</button>
            </div>
            {settings.palette === "custom" && (
              <div className="studio-colors">
                <label><span>{t("studio.foreground")}</span><input onChange={(event) => update({ foreground: event.target.value })} type="color" value={settings.foreground} /></label>
                <label><span>{t("studio.background")}</span><input onChange={(event) => update({ background: event.target.value })} type="color" value={settings.background} /></label>
              </div>
            )}
          </section>

          <section className="studio-section studio-section-row">
            <label><span>{t("studio.zoom")}</span><select aria-label={t("studio.zoom")} onChange={(event) => update({ zoom: Number(event.target.value) as StudioZoom })} value={settings.zoom}>{STUDIO_ZOOMS.map((zoom) => <option key={zoom} value={zoom}>{zoom}×</option>)}</select></label>
            <label><span>{t("studio.dpi")}</span><select aria-label={t("studio.dpi")} onChange={(event) => update({ dpi: Number(event.target.value) as StudioDpi })} value={settings.dpi}>{STUDIO_DPIS.map((dpi) => <option key={dpi} value={dpi}>{dpi}</option>)}</select></label>
            <button className="button ghost" onClick={() => { const defaults = loadStudioSettings(); setSettings({ ...defaults, ...structuredClone(defaultsReset()) }); }} type="button"><RotateCcw aria-hidden="true" size={14} /> {t("studio.reset")}</button>
          </section>
        </aside>

        <main className="studio-main" id="main-content" tabIndex={-1}>
          <div className={boardsClass} data-compare={twoBoards ? settings.compare : "single"} ref={boardsRef}>
            {(!twoBoards || settings.compare !== "blink" || !blinkB) && !showDiff && (
              <StudioBoard background={palette.background} dpi={settings.dpi} error={rendersA.error} foreground={palette.foreground} label={sourceLabel(settings.sourceA, sourceA)} lines={rendersA.lines} message={unavailableMessage(sourceA)} onLoupe={setLoupe} rendering={rendersA.rendering} zoom={settings.zoom} />
            )}
            {twoBoards && settings.compare === "blink" && blinkB && (
              <StudioBoard background={palette.background} dpi={settings.dpi} error={rendersB.error} foreground={palette.foreground} label={sourceLabel(settings.sourceB, sourceB)} lines={rendersB.lines} message={unavailableMessage(sourceB)} onLoupe={setLoupe} rendering={rendersB.rendering} zoom={settings.zoom} />
            )}
            {twoBoards && (settings.compare === "side" || settings.compare === "stack") && (
              <StudioBoard background={palette.background} dpi={settings.dpi} error={rendersB.error} foreground={palette.foreground} label={sourceLabel(settings.sourceB, sourceB)} lines={rendersB.lines} message={unavailableMessage(sourceB)} onLoupe={setLoupe} rendering={rendersB.rendering} zoom={settings.zoom} />
            )}
            {showDiff && (
              <StudioBoard against={rendersB.lines} background="#101216" dpi={settings.dpi} error={rendersA.error ?? rendersB.error} foreground="#E8ECF1" label={`${sourceLabel(settings.sourceA, sourceA)} ↔ ${sourceLabel(settings.sourceB, sourceB)}`} lines={rendersA.lines} message={unavailableMessage(sourceA) ?? unavailableMessage(sourceB)} onLoupe={setLoupe} rendering={rendersA.rendering || rendersB.rendering} zoom={settings.zoom} />
            )}
          </div>
          <footer className="studio-status">
            <span>{t("studio.strips", { count: stripCount })}</span>
            <span>{settings.zoom}× · {settings.dpi} DPI</span>
            <span className="studio-status-hint">{t("studio.loupeHint")}</span>
            <span className="app-statusbar-spacer" />
            {exportMessage && <span className="studio-export-message" aria-live="polite">{exportMessage}</span>}
            <button className="button secondary" onClick={() => void showNative()} type="button"><AppWindow aria-hidden="true" size={14} /> {t("studio.native")}</button>
            <button className="button primary" disabled={exporting || stripCount === 0} onClick={() => void exportPng()} type="button"><Download aria-hidden="true" size={14} /> {t("studio.export")}</button>
          </footer>
        </main>
      </div>
      <StudioLoupe state={loupe} />
    </div>
  );
}

function defaultsReset(): Partial<StudioSettings> {
  return { fonts: ["Segoe UI"], sizes: [9, 10, 11, 12, 14, 18, 24], styles: ["regular"], preset: "mixed", customText: presetTexts.mixed, palette: "theme", inverted: false, zoom: 1, dpi: 96 };
}
