import { useEffect, useMemo, useRef, useState, type PointerEvent as ReactPointerEvent } from "react";
import { previewImageUrl } from "../app/tauri";
import { useI18n } from "../i18n/i18n";
import type { SpecimenLine } from "../features/preview/useSpecimenRenders";
import type { StudioZoom } from "./studioModel";

const LOUPE_SIZE = 160;
const LOUPE_SCALE = 8;

interface StripImageProps {
  line: SpecimenLine;
  zoom: StudioZoom;
  /* Device pixels per CSS pixel of the strip at zoom 1. */
  scale: number;
  onLoupe: (state: LoupeState | null) => void;
}

export interface LoupeState {
  image: HTMLImageElement;
  /* Device-pixel coordinates in the image under the pointer. */
  x: number;
  y: number;
  /* Viewport position of the pointer. */
  clientX: number;
  clientY: number;
}

/* A strip at integer zoom: the bitmap is drawn onto a canvas with smoothing
   off, so every device pixel becomes a crisp zoom×zoom block. At zoom 1 the
   plain image is shown, sized to its device pixels. */
function StripImage({ line, zoom, scale, onLoupe }: StripImageProps) {
  const { t } = useI18n();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const imageRef = useRef<HTMLImageElement | null>(null);
  const [loaded, setLoaded] = useState(false);
  const source = previewImageUrl(line.result.imagePath);
  const width = line.result.width / scale;
  const height = line.result.height / scale;

  useEffect(() => {
    let active = true;
    const image = new Image();
    image.onload = () => {
      if (!active) return;
      imageRef.current = image;
      setLoaded(true);
      const canvas = canvasRef.current;
      if (!canvas) return;
      canvas.width = image.naturalWidth * zoom;
      canvas.height = image.naturalHeight * zoom;
      const context = canvas.getContext("2d");
      if (!context) return;
      context.imageSmoothingEnabled = false;
      context.drawImage(image, 0, 0, canvas.width, canvas.height);
    };
    image.src = source;
    return () => {
      active = false;
    };
  }, [source, zoom]);

  const track = (event: ReactPointerEvent<HTMLElement>) => {
    const image = imageRef.current;
    if (!image) return;
    const rect = event.currentTarget.getBoundingClientRect();
    const x = Math.floor(((event.clientX - rect.left) / rect.width) * image.naturalWidth);
    const y = Math.floor(((event.clientY - rect.top) / rect.height) * image.naturalHeight);
    onLoupe({ image, x, y, clientX: event.clientX, clientY: event.clientY });
  };

  if (zoom === 1) {
    return <img alt={t("profiles.previewImageAlt")} height={height} onPointerLeave={() => onLoupe(null)} onPointerMove={track} src={source} width={width} />;
  }
  return <canvas aria-label={t("profiles.previewImageAlt")} data-loaded={loaded} onPointerLeave={() => onLoupe(null)} onPointerMove={track} ref={canvasRef} role="img" style={{ width: width * zoom, height: height * zoom }} />;
}

/* Two strips of the same size drawn as a difference: pixels that match go
   dark, pixels that differ are lit in proportion to the difference. This is
   the honest way to show two rasterisations, since eyes miss a one-level
   change in a stem but a difference map does not. */
function DiffImage({ a, b, zoom, scale }: { a: SpecimenLine; b: SpecimenLine; zoom: StudioZoom; scale: number }) {
  const { t } = useI18n();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const width = Math.min(a.result.width, b.result.width);
  const height = Math.min(a.result.height, b.result.height);

  useEffect(() => {
    let active = true;
    const load = (path: string) => new Promise<HTMLImageElement>((resolve, reject) => {
      const image = new Image();
      image.onload = () => resolve(image);
      image.onerror = () => reject(new Error("image failed to load"));
      image.src = previewImageUrl(path);
    });
    void Promise.all([load(a.result.imagePath), load(b.result.imagePath)]).then(([imageA, imageB]) => {
      if (!active) return;
      const canvas = canvasRef.current;
      if (!canvas) return;
      const w = Math.min(imageA.naturalWidth, imageB.naturalWidth);
      const h = Math.min(imageA.naturalHeight, imageB.naturalHeight);
      const scratch = document.createElement("canvas");
      scratch.width = w;
      scratch.height = h;
      const context = scratch.getContext("2d", { willReadFrequently: true });
      if (!context) return;
      context.drawImage(imageA, 0, 0);
      const dataA = context.getImageData(0, 0, w, h);
      context.clearRect(0, 0, w, h);
      context.drawImage(imageB, 0, 0);
      const dataB = context.getImageData(0, 0, w, h);
      const out = context.createImageData(w, h);
      for (let index = 0; index < out.data.length; index += 4) {
        const dr = Math.abs(dataA.data[index] - dataB.data[index]);
        const dg = Math.abs(dataA.data[index + 1] - dataB.data[index + 1]);
        const db = Math.abs(dataA.data[index + 2] - dataB.data[index + 2]);
        const level = Math.min(255, Math.max(dr, dg, db) * 3);
        out.data[index] = Math.min(255, 16 + level);
        out.data[index + 1] = Math.min(255, 16 + Math.round(level * 0.85));
        out.data[index + 2] = Math.min(255, 24 + Math.round(level * 0.35));
        out.data[index + 3] = 255;
      }
      context.putImageData(out, 0, 0);
      canvas.width = w * zoom;
      canvas.height = h * zoom;
      const target = canvas.getContext("2d");
      if (!target) return;
      target.imageSmoothingEnabled = false;
      target.drawImage(scratch, 0, 0, canvas.width, canvas.height);
    }).catch(() => undefined);
    return () => {
      active = false;
    };
  }, [a.result.imagePath, b.result.imagePath, zoom]);

  return <canvas aria-label={t("studio.compare.diff")} className="studio-diff" ref={canvasRef} role="img" style={{ width: (width / scale) * zoom, height: (height / scale) * zoom }} />;
}

interface StudioBoardProps {
  label: string;
  lines: ReadonlyArray<SpecimenLine>;
  /* When set, each line is diffed against the line with the same strip key. */
  against?: ReadonlyArray<SpecimenLine>;
  zoom: StudioZoom;
  dpi: number;
  background: string;
  foreground: string;
  rendering: boolean;
  error: string | null;
  message?: string | null;
  onLoupe: (state: LoupeState | null) => void;
  boardRef?: (element: HTMLDivElement | null) => void;
}

function stripKey(key: string): string {
  return key.slice(key.indexOf("|") + 1);
}

export function StudioBoard({ label, lines, against, zoom, dpi, background, foreground, rendering, error, message, onLoupe, boardRef }: StudioBoardProps) {
  const { t } = useI18n();
  const scale = dpi / 96;
  const palette = !against && lines[0] ? lines[0].request : { background, foreground };
  const paired = useMemo(() => {
    if (!against) return null;
    const byKey = new Map(against.map((line) => [stripKey(line.key), line]));
    return lines.map((line) => ({ line, other: byKey.get(stripKey(line.key)) ?? null }));
  }, [against, lines]);

  return (
    <section aria-label={label} className="studio-board" data-rendering={rendering} ref={boardRef} style={{ background: palette.background, color: palette.foreground }}>
      <header className="studio-board-head"><span>{label}</span>{rendering && <span className="studio-board-busy">{t("studio.rendering")}</span>}</header>
      {message && <p className="studio-board-message">{message}</p>}
      {error && <p className="inline-error studio-board-message">{error}</p>}
      <div className="studio-board-strips">
        {paired
          ? paired.map(({ line, other }) => (
            <figure className="studio-strip" key={line.key}>
              <figcaption><span>{line.request.fontFace}</span><span>{line.request.fontSizePt}</span></figcaption>
              {other ? <DiffImage a={line} b={other} scale={scale} zoom={zoom} /> : <StripImage line={line} onLoupe={onLoupe} scale={scale} zoom={zoom} />}
            </figure>
          ))
          : lines.map((line) => (
            <figure className="studio-strip" key={line.key}>
              <figcaption><span>{line.request.fontFace}{line.request.bold ? " · B" : ""}{line.request.italic ? " · I" : ""}</span><span>{line.request.fontSizePt}</span></figcaption>
              <StripImage key={line.result.requestId} line={line} onLoupe={onLoupe} scale={scale} zoom={zoom} />
            </figure>
          ))}
      </div>
    </section>
  );
}

/* The loupe: an 8× nearest-neighbour view of the pixels under the pointer,
   drawn beside the cursor and kept inside the viewport. */
export function StudioLoupe({ state }: { state: LoupeState | null }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !state) return;
    const context = canvas.getContext("2d");
    if (!context) return;
    const radius = LOUPE_SIZE / LOUPE_SCALE / 2;
    context.imageSmoothingEnabled = false;
    context.fillStyle = "#000";
    context.fillRect(0, 0, LOUPE_SIZE, LOUPE_SIZE);
    context.drawImage(state.image, state.x - radius, state.y - radius, radius * 2, radius * 2, 0, 0, LOUPE_SIZE, LOUPE_SIZE);
    context.strokeStyle = "rgba(255, 80, 80, 0.9)";
    context.lineWidth = 1;
    context.strokeRect(LOUPE_SIZE / 2 - LOUPE_SCALE / 2 + 0.5, LOUPE_SIZE / 2 - LOUPE_SCALE / 2 + 0.5, LOUPE_SCALE, LOUPE_SCALE);
  }, [state]);
  if (!state) return null;
  const left = Math.min(window.innerWidth - LOUPE_SIZE - 12, state.clientX + 18);
  const top = Math.min(window.innerHeight - LOUPE_SIZE - 12, state.clientY + 18);
  return <canvas aria-hidden="true" className="studio-loupe" height={LOUPE_SIZE} ref={canvasRef} style={{ left, top }} width={LOUPE_SIZE} />;
}
