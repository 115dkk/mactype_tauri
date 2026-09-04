import { useEffect, useMemo, useRef, useState } from "react";
import type { PreviewEngine } from "../../app/model";
import { previewImageUrl } from "../../app/tauri";
import { useI18n } from "../../i18n/i18n";
import { specimenPalette } from "./specimenPalette";
import { specimenStripHeight, useSpecimenRenders, type SpecimenRequest } from "./useSpecimenRenders";

interface SpecimenBoardProps {
  profilePath: string | null;
  overrides?: Readonly<Record<string, number>>;
  engine?: PreviewEngine;
  fontFace: string;
  sizes: ReadonlyArray<number>;
  text: string;
  dark: boolean;
  /* Class of the board container; skins size it through their own CSS. */
  className?: string;
  /* Shows the point size beside each strip, the way a type specimen does. */
  labelled?: boolean;
  bold?: boolean;
  italic?: boolean;
}

/* A type-specimen board: one sample rendered by the helper at several sizes
   inside one canvas. The strips are requested at the canvas width and shown
   1:1, never resampled. */
export function SpecimenBoard({ profilePath, overrides, engine, fontFace, sizes, text, dark, className, labelled = true, bold, italic }: SpecimenBoardProps) {
  const { t } = useI18n();
  const canvasRef = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(0);
  const displayScale = window.devicePixelRatio || 1;
  const palette = specimenPalette(dark);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) setWidth(Math.max(0, Math.floor((entry.contentRect.width - (labelled ? 40 : 0)) / 8) * 8));
    });
    observer.observe(canvas);
    return () => observer.disconnect();
  }, [labelled]);

  const requests = useMemo<ReadonlyArray<SpecimenRequest>>(() => {
    if (!profilePath || width < 120) return [];
    const dpi = Math.round(96 * displayScale);
    return sizes.map((size) => ({
      key: `${size}`,
      profilePath,
      overrides: overrides ?? {},
      engine,
      text,
      fontFace,
      fontSizePt: size,
      widthPx: width * displayScale,
      heightPx: specimenStripHeight(text, size, dpi),
      dpi,
      foreground: palette.foreground,
      background: palette.background,
      bold,
      italic,
    }));
  }, [bold, displayScale, engine, fontFace, italic, overrides, palette.background, palette.foreground, profilePath, sizes, text, width]);

  const { lines, error } = useSpecimenRenders(requests);

  return (
    <div aria-label={t("profiles.previewAria")} className={className ?? "specimen-board"} data-dark={dark} data-empty={lines.length === 0} ref={canvasRef} role="img" style={{ background: palette.background, color: palette.foreground }}>
      {lines.length > 0 ? lines.map((line) => (
        <figure className="specimen-strip" data-size={line.request.fontSizePt} key={line.key}>
          {labelled && <figcaption>{line.request.fontSizePt}</figcaption>}
          <img alt={t("profiles.previewImageAlt")} height={line.result.height / displayScale} src={previewImageUrl(line.result.imagePath)} width={line.result.width / displayScale} />
        </figure>
      )) : sizes.map((size) => (
        <figure className="specimen-strip specimen-placeholder" data-size={size} key={size}>
          {labelled && <figcaption>{size}</figcaption>}
          <p style={{ fontFamily: `"${fontFace}", sans-serif`, fontSize: `${size}pt` }}>{text.split("\n")[0]}</p>
        </figure>
      ))}
      {error && <p className="inline-error specimen-error">{error}</p>}
    </div>
  );
}
