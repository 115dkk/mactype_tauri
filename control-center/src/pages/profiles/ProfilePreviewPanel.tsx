import { AlertTriangle, Columns2, Pencil, SlidersHorizontal } from "lucide-react";
import {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useRef,
  useState,
  type KeyboardEvent,
  type PointerEvent as ReactPointerEvent,
} from "react";
import type { NativePreviewMode, PreviewRequest, PreviewResult } from "../../app/model";
import {
  forcePreviewCrashForCi,
  previewImageUrl,
  renderProfilePreview,
  reportFrontendFailure,
  setNativePreview,
  verifyProfileWorkflowForCi,
} from "../../app/tauri";
import type { I18nValue } from "../../i18n/i18n";

const DEFAULT_PREVIEW_HEIGHT = 300;
const QUICK_PREVIEW_HEIGHT = 240;
const MIN_PREVIEW_HEIGHT = 128;
const MAX_PREVIEW_HEIGHT = 640;
/* The undocked preview is a bottom panel sharing its column with the settings
   form, so it never grows past the room this leaves the form. */
const MIN_SETTINGS_HEIGHT = 240;
/* The preview helper rejects bitmaps below 64 device pixels. */
const MIN_STRIP_HEIGHT = 64;

/** One rendered line of the preview stack (legacy Tuner shows sample groups). */
export interface PreviewVariant {
  key: string;
  label: string | null;
  bold?: boolean;
  italic?: boolean;
  foreground?: string;
  /** Fixed sample text; falls back to the editable sample when omitted. */
  text?: string;
}

type CompareSide = "saved" | "edited";

interface PreviewLine {
  key: string;
  label: string | null;
  side: CompareSide | null;
  result: PreviewResult;
}

interface PendingBatch {
  generation: number;
  batchId: number;
  requests: ReadonlyArray<{ key: string; label: string | null; side: CompareSide | null; request: PreviewRequest }>;
}

export interface ProfilePreviewHandle {
  show: () => void;
}

interface ProfilePreviewPanelProps {
  ciSmoke: boolean;
  docked: boolean;
  error: string | null;
  fontFace: string;
  fontFamilies: ReadonlyArray<string>;
  fontOptionLabel: (font: string) => string;
  mode: "quick" | "advanced";
  onError: (message: string | null) => void;
  onFontFaceChange: (font: string) => void;
  onPreviewReady?: () => void;
  profilePath: string | null;
  savedValues?: Readonly<Record<string, number>>;
  t: I18nValue["t"];
  values: Record<string, number>;
  variants: ReadonlyArray<PreviewVariant>;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/* One palette drives the rendered strips and the helper-owned native window,
   so the background choice means the same thing in both places. */
function previewPalette(dark: boolean): { foreground: string; background: string } {
  return dark
    ? { foreground: "#F1F3F5", background: "#171A1F" }
    : { foreground: "#181D23", background: "#EEF1F4" };
}

/* The helper rejects bitmaps above 2048 device pixels; stay under it at 2x. */
const MAX_STRIP_HEIGHT = 1000;

/* A strip is drawn at the width we ask for and the layout caps it at the
   canvas, so a bitmap wider than the canvas gets resampled down and the reader
   sees glyphs smaller than the size they picked. Asking for the canvas width
   instead keeps every layout at true size, and because the helper draws with
   ETO_CLIPPED rather than wrapping, the sample is broken into fitting lines
   here. */
const SAMPLE_INSET = 18;
/* Browser metrics and the helper's GDI metrics disagree by a little, so the
   last word keeps a margin rather than risking the clip. */
const SAMPLE_RIGHT_MARGIN = 12;
const MIN_SAMPLE_WIDTH = 220;
/* Follow the canvas in steps so a drag across the window edge does not churn
   state on every pixel. */
const SAMPLE_WIDTH_STEP = 8;
/* Latin breaks at spaces; CJK writes without them, so every syllable or
   ideograph is its own break opportunity. */
const CJK = "\\u1100-\\u11FF\\u2E80-\\u303F\\u3040-\\u30FF\\u3400-\\u4DBF\\u4E00-\\u9FFF\\uA960-\\uA97F\\uAC00-\\uD7FF\\uF900-\\uFAFF\\uFF00-\\uFF60";
const SAMPLE_TOKENS = new RegExp(`[${CJK}]\\s*|[^\\s${CJK}]+\\s*|\\s+`, "gu");

let sampleMeasureContext: CanvasRenderingContext2D | null | undefined;

function sampleMeasurer(fontFace: string, fontSizePt: number): ((text: string) => number) | null {
  if (sampleMeasureContext === undefined) sampleMeasureContext = document.createElement("canvas").getContext("2d");
  const context = sampleMeasureContext;
  if (!context) return null;
  context.font = `${(fontSizePt * 96) / 72}px "${fontFace}", sans-serif`;
  return (text: string) => context.measureText(text).width;
}

function wrapSampleLine(line: string, measure: (text: string) => number, room: number): ReadonlyArray<string> {
  const tokens = line.match(SAMPLE_TOKENS);
  if (!tokens) return [line];
  const wrapped: string[] = [];
  let current = "";
  for (const token of tokens) {
    if (current && measure((current + token).trimEnd()) > room) {
      wrapped.push(current.trimEnd());
      current = token.trimStart();
    } else {
      current += token;
    }
  }
  wrapped.push(current.trimEnd());
  return wrapped;
}

function wrapSample(text: string, fontFace: string, fontSizePt: number, widthPx: number): string {
  const room = widthPx - SAMPLE_INSET - SAMPLE_RIGHT_MARGIN;
  const measure = room > 0 ? sampleMeasurer(fontFace, fontSizePt) : null;
  if (!measure) return text;
  return text.split("\n").flatMap((line) => wrapSampleLine(line, measure, room)).join("\n");
}

function stripHeightFor(text: string, fontSize: number): number {
  const lines = Math.max(1, text.split("\n").length);
  const lineSpacing = Math.max(22, Math.round(fontSize * 2));
  return Math.min(MAX_STRIP_HEIGHT, Math.max(MIN_STRIP_HEIGHT, lines * lineSpacing + Math.round(fontSize * 0.7) + 10));
}

export const ProfilePreviewPanel = forwardRef<ProfilePreviewHandle, ProfilePreviewPanelProps>(function ProfilePreviewPanel({
  ciSmoke,
  docked,
  error,
  fontFace,
  fontFamilies,
  fontOptionLabel,
  mode,
  onError,
  onFontFaceChange,
  onPreviewReady,
  profilePath,
  savedValues,
  t,
  values,
  variants,
}, ref) {
  const [fontSize, setFontSize] = useState(14);
  const [darkPreview, setDarkPreview] = useState(false);
  const [sampleText, setSampleText] = useState(() => t("profiles.sampleText"));
  const [previewStack, setPreviewStack] = useState<ReadonlyArray<PreviewLine>>([]);
  const [nativeVisible, setNativeVisible] = useState(false);
  const [nativeMode, setNativeMode] = useState<NativePreviewMode>("default");
  const [previewHeight, setPreviewHeight] = useState(DEFAULT_PREVIEW_HEIGHT);
  const [sampleWidth, setSampleWidth] = useState(0);
  const [sampleEditorOpen, setSampleEditorOpen] = useState(false);
  const [comparing, setComparing] = useState(false);
  const hasUnsavedEdits = savedValues !== undefined
    && Object.keys(values).some((key) => values[key] !== savedValues[key]);
  /* Only the comparing state pulls the saved snapshot into the render batch:
     while comparison is off this stays undefined, so a fresh savedValues
     object from the document cannot retrigger the preview round-trip. */
  const compareOverrides = comparing ? savedValues : undefined;
  /* Saving makes both sides identical, so comparison stops paying for the
     extra render rather than showing the same strip twice. */
  useEffect(() => {
    if (!hasUnsavedEdits) setComparing(false);
  }, [hasUnsavedEdits]);
  const previousDefaultSample = useRef(sampleText);
  const canvasRef = useRef<HTMLDivElement>(null);
  const previewPanelRef = useRef<HTMLElement>(null);
  const resizeStart = useRef<{ pointerId: number; y: number; height: number } | null>(null);
  const pendingPreview = useRef<PendingBatch | null>(null);
  const previewRunning = useRef(false);
  const mounted = useRef(false);
  const generation = useRef(0);
  const batchCounter = useRef(0);
  const newestBatch = useRef(0);
  const restartVerified = useRef(false);
  const ciReadyRequestId = useRef<number | null>(null);
  const ciWorkflowVerified = useRef(false);

  const isCurrentGeneration = useCallback((candidate: number) => mounted.current && generation.current === candidate, []);

  useEffect(() => {
    mounted.current = true;
    generation.current += 1;
    return () => {
      mounted.current = false;
      generation.current += 1;
      pendingPreview.current = null;
    };
  }, []);

  useImperativeHandle(ref, () => ({
    show() {
      previewPanelRef.current?.scrollIntoView({ block: "center" });
      previewPanelRef.current?.focus({ preventScroll: true });
    },
  }), []);

  useEffect(() => {
    if (mode === "quick") setPreviewHeight((current) => Math.min(current, QUICK_PREVIEW_HEIGHT));
  }, [mode]);

  useEffect(() => {
    const available = previewPanelRef.current?.parentElement?.clientHeight;
    if (!available) return;
    const largest = Math.max(MIN_PREVIEW_HEIGHT, Math.min(MAX_PREVIEW_HEIGHT, available - MIN_SETTINGS_HEIGHT));
    setPreviewHeight((current) => Math.min(current, largest));
  }, []);

  /* Docking moves the canvas between a full-width bottom panel and a narrow
     right column, so the strips are re-rendered for the width they land in. */
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const room = Math.floor(entry.contentRect.width / SAMPLE_WIDTH_STEP) * SAMPLE_WIDTH_STEP;
        setSampleWidth(Math.max(MIN_SAMPLE_WIDTH, room));
      }
    });
    observer.observe(canvas);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    const nextDefault = t("profiles.sampleText");
    setSampleText((current) => current === previousDefaultSample.current ? nextDefault : current);
    previousDefaultSample.current = nextDefault;
  }, [t]);

  const maximumPreviewHeight = useCallback(() => Math.max(
    MIN_PREVIEW_HEIGHT,
    Math.min(MAX_PREVIEW_HEIGHT, (previewPanelRef.current?.parentElement?.clientHeight ?? MAX_PREVIEW_HEIGHT + MIN_SETTINGS_HEIGHT) - MIN_SETTINGS_HEIGHT),
  ), []);
  const clampPreviewHeight = useCallback((height: number) => Math.min(maximumPreviewHeight(), Math.max(MIN_PREVIEW_HEIGHT, height)), [maximumPreviewHeight]);

  const drainPreviewQueue = useCallback(async () => {
    if (previewRunning.current) return;
    previewRunning.current = true;
    try {
      while (pendingPreview.current) {
        const pending = pendingPreview.current;
        pendingPreview.current = null;
        const lines: PreviewLine[] = [];
        let aborted = false;
        for (const entry of pending.requests) {
          try {
            const rendered = await renderProfilePreview(entry.request);
            if (!isCurrentGeneration(pending.generation)) {
              aborted = true;
              break;
            }
            if (!rendered) continue;
            lines.push({ key: entry.key, label: entry.label, side: entry.side, result: rendered });
            /* A newer value is already waiting. Stop spending helper work on
               this obsolete batch and, crucially, never expose its partial
               stack to the canvas. */
            if (pending.batchId < batchCounter.current) {
              aborted = true;
              break;
            }
          } catch (caught: unknown) {
            if (isCurrentGeneration(pending.generation)) onError(errorMessage(caught));
            aborted = true;
            break;
          }
        }
        if (aborted || lines.length !== pending.requests.length || pending.batchId < newestBatch.current) continue;
        if (ciSmoke && !restartVerified.current) {
          restartVerified.current = true;
          await forcePreviewCrashForCi();
          if (!isCurrentGeneration(pending.generation)) continue;
          pendingPreview.current = pending;
          continue;
        }
        /* Saved/edited RGB comparison can contain eight helper renders. Keep
           the last complete image visible until the entire newest batch is
           ready, then swap it in once so the stack cannot flash through
           incomplete 1..7-line layouts. */
        if (pending.batchId < batchCounter.current) continue;
        newestBatch.current = pending.batchId;
        setPreviewStack(lines);
        onError(null);
        if (ciSmoke) ciReadyRequestId.current = lines[lines.length - 1].result.requestId;
        else onPreviewReady?.();
      }
    } finally {
      previewRunning.current = false;
    }
  }, [ciSmoke, isCurrentGeneration, onError, onPreviewReady]);

  useEffect(() => {
    if (!profilePath || variants.length === 0 || sampleWidth === 0) return undefined;
    const requestGeneration = generation.current;
    if (!isCurrentGeneration(requestGeneration)) return undefined;
    const displayScale = window.devicePixelRatio || 1;
    const width = sampleWidth;
    /* Comparing means rendering each variant twice, so the saved side is
       only requested while the reader has comparison switched on. Captions
       are resolved at render time; keeping them out of the batch means the
       translator identity cannot retrigger a render round-trip. */
    const sides: ReadonlyArray<{ suffix: string; overrides: Record<string, number>; side: CompareSide | null }> = compareOverrides
      ? [{ suffix: ":saved", overrides: compareOverrides, side: "saved" },
         { suffix: ":edited", overrides: values, side: "edited" }]
      : [{ suffix: "", overrides: values, side: null }];
    pendingPreview.current = {
      generation: requestGeneration,
      batchId: ++batchCounter.current,
      requests: variants.flatMap((variant) => {
        const text = wrapSample(variant.text ?? sampleText, fontFace, fontSize, width);
        return sides.map((side) => ({
          key: `${variant.key}${side.suffix}`,
          label: variant.label,
          side: side.side,
          request: {
            profilePath,
            overrides: side.overrides,
            displayScale,
            sample: {
              text,
              fontFace,
              fontSizePt: fontSize,
              widthPx: Math.round(width * displayScale),
              heightPx: Math.round(stripHeightFor(text, fontSize) * displayScale),
              dpi: Math.round(96 * displayScale),
              foreground: variant.foreground ?? previewPalette(darkPreview).foreground,
              background: previewPalette(darkPreview).background,
              bold: variant.bold ?? false,
              italic: variant.italic ?? false,
            },
          },
        }));
      }),
    };
    /* The queue itself coalesces in-flight work to the latest batch, so a
       trailing timer only makes continuous wheel input feel unresponsive. */
    void drainPreviewQueue();
    return undefined;
  }, [compareOverrides, darkPreview, drainPreviewQueue, fontFace, fontSize, isCurrentGeneration, profilePath, sampleText, sampleWidth, values, variants]);

  const resizePreviewFromKeyboard = (event: KeyboardEvent<HTMLDivElement>) => {
    const increments: Partial<Record<string, number>> = { ArrowUp: 16, ArrowDown: -16, PageUp: 48, PageDown: -48 };
    const increment = increments[event.key];
    if (event.key === "Home") {
      event.preventDefault();
      setPreviewHeight(MIN_PREVIEW_HEIGHT);
    } else if (event.key === "End") {
      event.preventDefault();
      setPreviewHeight(maximumPreviewHeight());
    } else if (increment !== undefined) {
      event.preventDefault();
      setPreviewHeight((current) => clampPreviewHeight(current + increment));
    }
  };
  const startPreviewResize = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.currentTarget.setPointerCapture(event.pointerId);
    resizeStart.current = { pointerId: event.pointerId, y: event.clientY, height: previewHeight };
  };
  const continuePreviewResize = (event: ReactPointerEvent<HTMLDivElement>) => {
    const start = resizeStart.current;
    if (!start || start.pointerId !== event.pointerId) return;
    setPreviewHeight(clampPreviewHeight(start.height + start.y - event.clientY));
  };
  const finishPreviewResize = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (resizeStart.current?.pointerId !== event.pointerId) return;
    resizeStart.current = null;
    if (event.currentTarget.hasPointerCapture(event.pointerId)) event.currentTarget.releasePointerCapture(event.pointerId);
  };

  /* The native window carries its own colours rather than inheriting them from
     whichever strip the helper rendered last; otherwise the window silently
     shows the final variant's style (bold, or a channel-pure colour). */
  const applyNativePreview = useCallback(async (visible: boolean, mode: NativePreviewMode, dark: boolean) => {
    const requestGeneration = generation.current;
    try {
      const nowVisible = await setNativePreview(visible, {
        mode,
        listingText: t("profiles.samplePangram"),
        ...previewPalette(dark),
      });
      if (isCurrentGeneration(requestGeneration)) setNativeVisible(nowVisible);
    } catch (caught: unknown) {
      if (isCurrentGeneration(requestGeneration)) onError(errorMessage(caught));
    }
  }, [isCurrentGeneration, onError, t]);

  const toggleNativePreview = () => applyNativePreview(!nativeVisible, nativeMode, darkPreview);

  /* The legacy-listing choice repaints an already-open native window in place. */
  const changeNativeMode = (mode: NativePreviewMode) => {
    setNativeMode(mode);
    if (nativeVisible) void applyNativePreview(true, mode, darkPreview);
  };

  /* An open native window follows the background choice without reopening. */
  const toggleDarkPreview = () => {
    const dark = !darkPreview;
    setDarkPreview(dark);
    if (nativeVisible) void applyNativePreview(true, nativeMode, dark);
  };

  const verifyCiWorkflow = (line: PreviewLine) => {
    if (!ciSmoke || ciReadyRequestId.current !== line.result.requestId || ciWorkflowVerified.current) return;
    ciWorkflowVerified.current = true;
    const requestGeneration = generation.current;
    void verifyProfileWorkflowForCi()
      .then(() => {
        if (isCurrentGeneration(requestGeneration)) onPreviewReady?.();
      })
      .catch((caught: unknown) => {
        if (!isCurrentGeneration(requestGeneration)) return;
        const message = errorMessage(caught);
        onError(message);
        void reportFrontendFailure("profiles", message);
      });
  };

  const displayScale = window.devicePixelRatio || 1;
  const fallbackSample = t("profiles.sampleText").split("\n");

  return (
    <section className="preview-panel" aria-labelledby="preview-title" data-compact={!docked && previewHeight < 220} ref={previewPanelRef} style={docked ? undefined : { height: previewHeight }} tabIndex={-1}>
      {!docked && <div
        aria-label={t("profiles.previewResize")}
        aria-orientation="horizontal"
        aria-valuemax={MAX_PREVIEW_HEIGHT}
        aria-valuemin={MIN_PREVIEW_HEIGHT}
        aria-valuenow={Math.round(previewHeight)}
        className="preview-resizer"
        onKeyDown={resizePreviewFromKeyboard}
        onPointerCancel={finishPreviewResize}
        onPointerDown={startPreviewResize}
        onPointerMove={continuePreviewResize}
        onPointerUp={finishPreviewResize}
        role="separator"
        tabIndex={0}
      ><span aria-hidden="true" /></div>}
      <div className="preview-toolbar">
        <div><SlidersHorizontal aria-hidden="true" size={17} /><h2 id="preview-title">{t("profiles.preview")}</h2></div>
        <div className="preview-controls">
          <select aria-label={t("profiles.previewFont")} onChange={(event) => onFontFaceChange(event.target.value)} value={fontFace}>{fontFamilies.map((font) => <option key={font} value={font}>{fontOptionLabel(font)}</option>)}</select>
          <select aria-label={t("profiles.previewSize")} onChange={(event) => setFontSize(Number(event.target.value))} value={fontSize}><option value="12">12 pt</option><option value="14">14 pt</option><option value="18">18 pt</option></select>
          <button aria-expanded={sampleEditorOpen} className="text-action" onClick={() => setSampleEditorOpen((current) => !current)} type="button"><Pencil aria-hidden="true" size={14} /> {t("profiles.editSample")}</button>
          <button className="text-action" onClick={toggleDarkPreview} type="button">{darkPreview ? t("profiles.lightBackground") : t("profiles.darkBackground")}</button>
        </div>
      </div>
      {sampleEditorOpen && <textarea className="sample-input" aria-label={t("profiles.sampleAria")} onChange={(event) => setSampleText(event.target.value)} rows={2} value={sampleText} />}
      <div className="preview-canvas" data-dark={darkPreview} data-stack={previewStack.length > 0} ref={canvasRef} role="img" aria-label={t("profiles.previewAria")}>
        {previewStack.length > 0 ? previewStack.map((line) => (
          <figure className="preview-strip" data-variant={line.key} key={line.key}>
            {(line.label || line.side) && <figcaption>{[line.label, line.side && t(line.side === "saved" ? "profiles.compareSaved" : "profiles.compareEdited")].filter(Boolean).join(" · ")}</figcaption>}
            <img
              alt={t("profiles.previewImageAlt")}
              height={line.result.height / displayScale}
              onLoad={() => verifyCiWorkflow(line)}
              src={previewImageUrl(line.result.imagePath)}
              width={line.result.width / displayScale}
            />
          </figure>
        )) : fallbackSample.map((line) => <p key={line}>{line}</p>)}
      </div>
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
      <div className="preview-footer">
        <button aria-pressed={comparing} className="text-action" disabled={!hasUnsavedEdits} onClick={() => setComparing((current) => !current)} title={hasUnsavedEdits ? undefined : t("profiles.compareUnavailable")} type="button"><Columns2 aria-hidden="true" size={14} /> {t("profiles.compareToggle")}</button>
        <select aria-label={t("profiles.nativeDisplayMode")} onChange={(event) => changeNativeMode(event.target.value === "listing" ? "listing" : "default")} value={nativeMode}>
          <option value="default">{t("profiles.nativeDisplayDefault")}</option>
          <option value="listing">{t("profiles.nativeDisplayListing")}</option>
        </select>
        <button className="text-action" onClick={() => void toggleNativePreview()} type="button">{nativeVisible ? t("profiles.closeNative") : t("profiles.openNative")}</button>
      </div>
    </section>
  );
});
