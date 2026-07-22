import { AlertTriangle, SlidersHorizontal } from "lucide-react";
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

const DEFAULT_PREVIEW_HEIGHT = 380;
const QUICK_PREVIEW_HEIGHT = 320;
const MIN_PREVIEW_HEIGHT = 128;
const MAX_PREVIEW_HEIGHT = 640;
const MIN_SETTINGS_HEIGHT = 160;

export interface ProfilePreviewHandle {
  show: () => void;
}

interface ProfilePreviewPanelProps {
  ciSmoke: boolean;
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
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export const ProfilePreviewPanel = forwardRef<ProfilePreviewHandle, ProfilePreviewPanelProps>(function ProfilePreviewPanel({
  ciSmoke,
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
}, ref) {
  const [fontSize, setFontSize] = useState(14);
  const [darkPreview, setDarkPreview] = useState(false);
  const [sampleText, setSampleText] = useState(() => t("profiles.sampleText"));
  const [preview, setPreview] = useState<PreviewResult | null>(null);
  const [nativeVisible, setNativeVisible] = useState(false);
  const [previewHeight, setPreviewHeight] = useState(DEFAULT_PREVIEW_HEIGHT);
  const previousDefaultSample = useRef(sampleText);
  const canvasRef = useRef<HTMLDivElement>(null);
  const previewPanelRef = useRef<HTMLElement>(null);
  const resizeStart = useRef<{ pointerId: number; y: number; height: number } | null>(null);
  const pendingPreview = useRef<{ generation: number; request: PreviewRequest } | null>(null);
  const previewRunning = useRef(false);
  const mounted = useRef(false);
  const generation = useRef(0);
  const newestResponse = useRef(0);
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

  const drainPreviewQueue = useCallback(async () => {
    if (previewRunning.current) return;
    previewRunning.current = true;
    try {
      while (pendingPreview.current) {
        const pending = pendingPreview.current;
        pendingPreview.current = null;
        try {
          const rendered = await renderProfilePreview(pending.request);
          if (!isCurrentGeneration(pending.generation)) continue;
          if (rendered && rendered.requestId > newestResponse.current) {
            newestResponse.current = rendered.requestId;
            setPreview(rendered);
            onError(null);
            if (ciSmoke && !restartVerified.current) {
              restartVerified.current = true;
              await forcePreviewCrashForCi();
              if (!isCurrentGeneration(pending.generation)) continue;
              pendingPreview.current = pending;
              continue;
            }
            if (ciSmoke) ciReadyRequestId.current = rendered.requestId;
            else onPreviewReady?.();
          }
        } catch (caught: unknown) {
          if (isCurrentGeneration(pending.generation)) onError(errorMessage(caught));
        }
      }
    } finally {
      previewRunning.current = false;
    }
  }, [ciSmoke, isCurrentGeneration, onError, onPreviewReady]);

  useEffect(() => {
    if (!profilePath) return undefined;
    const requestGeneration = generation.current;
    const timer = window.setTimeout(() => {
      if (!isCurrentGeneration(requestGeneration)) return;
      const displayScale = window.devicePixelRatio || 1;
      const width = Math.max(320, canvasRef.current?.clientWidth ?? 760);
      const height = Math.max(72, canvasRef.current?.clientHeight ?? 180);
      pendingPreview.current = {
        generation: requestGeneration,
        request: {
          profilePath,
          overrides: values,
          displayScale,
          sample: {
            text: sampleText,
            fontFace,
            fontSizePt: fontSize,
            widthPx: Math.round(width * displayScale),
            heightPx: Math.round(height * displayScale),
            dpi: Math.round(96 * displayScale),
            foreground: darkPreview ? "#F1F3F5" : "#181D23",
            background: darkPreview ? "#171A1F" : "#EEF1F4",
          },
        },
      };
      void drainPreviewQueue();
    }, 40);
    return () => window.clearTimeout(timer);
  }, [darkPreview, drainPreviewQueue, fontFace, fontSize, isCurrentGeneration, previewHeight, profilePath, sampleText, values]);

  const maximumPreviewHeight = () => Math.max(
    MIN_PREVIEW_HEIGHT,
    Math.min(MAX_PREVIEW_HEIGHT, (previewPanelRef.current?.parentElement?.clientHeight ?? MAX_PREVIEW_HEIGHT + MIN_SETTINGS_HEIGHT) - MIN_SETTINGS_HEIGHT),
  );
  const clampPreviewHeight = (height: number) => Math.min(maximumPreviewHeight(), Math.max(MIN_PREVIEW_HEIGHT, height));
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

  const toggleNativePreview = async () => {
    const requestGeneration = generation.current;
    try {
      const visible = await setNativePreview(!nativeVisible);
      if (isCurrentGeneration(requestGeneration)) setNativeVisible(visible);
    } catch (caught: unknown) {
      if (isCurrentGeneration(requestGeneration)) onError(errorMessage(caught));
    }
  };

  const displayScale = window.devicePixelRatio || 1;
  const fallbackSample = t("profiles.sampleText").split("\n");

  return (
    <section className="preview-panel" aria-labelledby="preview-title" data-compact={previewHeight < 220} ref={previewPanelRef} style={{ height: previewHeight }} tabIndex={-1}>
      <div
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
      ><span aria-hidden="true" /></div>
      <div className="preview-toolbar">
        <div><SlidersHorizontal aria-hidden="true" size={17} /><h2 id="preview-title">{t("profiles.preview")}</h2></div>
        <div className="preview-controls">
          <select aria-label={t("profiles.previewFont")} onChange={(event) => onFontFaceChange(event.target.value)} value={fontFace}>{fontFamilies.map((font) => <option key={font} value={font}>{fontOptionLabel(font)}</option>)}</select>
          <select aria-label={t("profiles.previewSize")} onChange={(event) => setFontSize(Number(event.target.value))} value={fontSize}><option value="12">12 pt</option><option value="14">14 pt</option><option value="18">18 pt</option></select>
          <button className="text-action" onClick={() => setDarkPreview((current) => !current)} type="button">{darkPreview ? t("profiles.lightBackground") : t("profiles.darkBackground")}</button>
        </div>
      </div>
      <textarea className="sample-input" aria-label={t("profiles.sampleAria")} onChange={(event) => setSampleText(event.target.value)} rows={2} value={sampleText} />
      <div className="preview-canvas" data-dark={darkPreview} ref={canvasRef} role="img" aria-label={t("profiles.previewAria")}>
        {preview ? (
          <img
            alt={t("profiles.previewImageAlt")}
            height={preview.height / displayScale}
            onLoad={() => {
              if (ciSmoke && ciReadyRequestId.current === preview.requestId && !ciWorkflowVerified.current) {
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
              }
            }}
            src={previewImageUrl(preview.imagePath)}
            width={preview.width / displayScale}
          />
        ) : <><p>{fallbackSample[0]}</p><p>{fallbackSample[1]}</p><span>{t("profiles.helperWaiting")}</span></>}
      </div>
      {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
      <div className="preview-footer">
        <span>{preview ? t("profiles.previewRequest", { request: preview.requestId, dpi: preview.dpi, elapsed: preview.elapsedMs }) : t("profiles.previewReady")}</span>
        <button className="text-action" onClick={() => void toggleNativePreview()} type="button">{nativeVisible ? t("profiles.closeNative") : t("profiles.openNative")}</button>
      </div>
    </section>
  );
});
