import { AlertTriangle, Pencil, SlidersHorizontal } from "lucide-react";
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
import type { PreviewRequest, PreviewResult } from "../../app/model";
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
const QUICK_PREVIEW_HEIGHT = 280;
const MIN_PREVIEW_HEIGHT = 128;
const MAX_PREVIEW_HEIGHT = 640;
const MIN_SETTINGS_HEIGHT = 160;
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

interface PreviewLine {
  key: string;
  label: string | null;
  result: PreviewResult;
}

interface PendingBatch {
  generation: number;
  batchId: number;
  requests: ReadonlyArray<{ key: string; label: string | null; request: PreviewRequest }>;
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
  t: I18nValue["t"];
  values: Record<string, number>;
  variants: ReadonlyArray<PreviewVariant>;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/* The helper rejects bitmaps above 2048 device pixels; stay under it at 2x. */
const MAX_STRIP_HEIGHT = 1000;

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
  t,
  values,
  variants,
}, ref) {
  const [fontSize, setFontSize] = useState(14);
  const [darkPreview, setDarkPreview] = useState(false);
  const [sampleText, setSampleText] = useState(() => t("profiles.sampleText"));
  const [previewStack, setPreviewStack] = useState<ReadonlyArray<PreviewLine>>([]);
  const [nativeVisible, setNativeVisible] = useState(false);
  const [previewHeight, setPreviewHeight] = useState(DEFAULT_PREVIEW_HEIGHT);
  const [sampleEditorOpen, setSampleEditorOpen] = useState(false);
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

  /* Step-aware stacks may need more room (four LCD lines); grow the panel by
     the measured canvas overflow so the last line stays visible instead of
     being clipped. A deliberate manual resize wins until the stack shape
     changes again. */
  const manualResize = useRef(false);
  useEffect(() => {
    manualResize.current = false;
  }, [variants.length]);
  useEffect(() => {
    if (docked || manualResize.current) return;
    const canvas = canvasRef.current;
    if (!canvas || previewStack.length === 0) return;
    const overflow = canvas.scrollHeight - canvas.clientHeight;
    if (overflow > 0) setPreviewHeight((current) => clampPreviewHeight(current + overflow));
  }, [clampPreviewHeight, docked, previewStack]);

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
            lines.push({ key: entry.key, label: entry.label, result: rendered });
            if (pending.batchId >= newestBatch.current) {
              newestBatch.current = pending.batchId;
              setPreviewStack([...lines]);
              onError(null);
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
        if (ciSmoke) ciReadyRequestId.current = lines[lines.length - 1].result.requestId;
        else onPreviewReady?.();
      }
    } finally {
      previewRunning.current = false;
    }
  }, [ciSmoke, isCurrentGeneration, onError, onPreviewReady]);

  useEffect(() => {
    if (!profilePath || variants.length === 0) return undefined;
    const requestGeneration = generation.current;
    const timer = window.setTimeout(() => {
      if (!isCurrentGeneration(requestGeneration)) return;
      const displayScale = window.devicePixelRatio || 1;
      const width = Math.max(320, canvasRef.current?.clientWidth ?? 760);
      pendingPreview.current = {
        generation: requestGeneration,
        batchId: ++batchCounter.current,
        requests: variants.map((variant) => {
          const text = variant.text ?? sampleText;
          return {
            key: variant.key,
            label: variant.label,
            request: {
              profilePath,
              overrides: values,
              displayScale,
              sample: {
                text,
                fontFace,
                fontSizePt: fontSize,
                widthPx: Math.round(width * displayScale),
                heightPx: Math.round(stripHeightFor(text, fontSize) * displayScale),
                dpi: Math.round(96 * displayScale),
                foreground: variant.foreground ?? (darkPreview ? "#F1F3F5" : "#181D23"),
                background: darkPreview ? "#171A1F" : "#EEF1F4",
                bold: variant.bold ?? false,
                italic: variant.italic ?? false,
              },
            },
          };
        }),
      };
      void drainPreviewQueue();
    }, 40);
    return () => window.clearTimeout(timer);
  }, [darkPreview, drainPreviewQueue, fontFace, fontSize, isCurrentGeneration, profilePath, sampleText, values, variants]);

  const resizePreviewFromKeyboard = (event: KeyboardEvent<HTMLDivElement>) => {
    const increments: Partial<Record<string, number>> = { ArrowUp: 16, ArrowDown: -16, PageUp: 48, PageDown: -48 };
    const increment = increments[event.key];
    if (event.key === "Home" || event.key === "End" || increment !== undefined) manualResize.current = true;
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
    manualResize.current = true;
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

  const toggleNativePreview = async () => {
    const requestGeneration = generation.current;
    try {
      const visible = await setNativePreview(!nativeVisible);
      if (isCurrentGeneration(requestGeneration)) setNativeVisible(visible);
    } catch (caught: unknown) {
      if (isCurrentGeneration(requestGeneration)) onError(errorMessage(caught));
    }
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
          <button className="text-action" onClick={() => setDarkPreview((current) => !current)} type="button">{darkPreview ? t("profiles.lightBackground") : t("profiles.darkBackground")}</button>
        </div>
      </div>
      {sampleEditorOpen && <textarea className="sample-input" aria-label={t("profiles.sampleAria")} onChange={(event) => setSampleText(event.target.value)} rows={2} value={sampleText} />}
      <div className="preview-canvas" data-dark={darkPreview} data-stack={previewStack.length > 0} ref={canvasRef} role="img" aria-label={t("profiles.previewAria")}>
        {previewStack.length > 0 ? previewStack.map((line) => (
          <figure className="preview-strip" data-variant={line.key} key={line.key}>
            {line.label && <figcaption>{line.label}</figcaption>}
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
        <button className="text-action" onClick={() => void toggleNativePreview()} type="button">{nativeVisible ? t("profiles.closeNative") : t("profiles.openNative")}</button>
      </div>
    </section>
  );
});
