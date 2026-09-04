import { previewImageUrl } from "../app/tauri";
import type { SpecimenLine } from "../features/preview/useSpecimenRenders";

interface ExportBoard {
  label: string;
  lines: ReadonlyArray<SpecimenLine>;
}

const MARGIN = 24;
const GAP = 8;
const LABEL_HEIGHT = 22;

function loadImage(path: string): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error("specimen image failed to load"));
    image.src = previewImageUrl(path);
  });
}

/* Composes the visible boards into one PNG at device resolution: boards side
   by side, each strip at 1:1 with its font and size written above it. The
   result is the base64 body the backend expects. */
export async function composeStudioPng(boards: ReadonlyArray<ExportBoard>, background: string, foreground: string): Promise<string> {
  const loaded = await Promise.all(boards.map(async (board) => ({
    label: board.label,
    strips: await Promise.all(board.lines.map(async (line) => ({ line, image: await loadImage(line.result.imagePath) }))),
  })));
  const columnWidths = loaded.map((board) => Math.max(320, ...board.strips.map((strip) => strip.image.naturalWidth)));
  const columnHeights = loaded.map((board) => board.strips.reduce((total, strip) => total + LABEL_HEIGHT + strip.image.naturalHeight + GAP, LABEL_HEIGHT + GAP));
  const width = MARGIN * 2 + columnWidths.reduce((total, w) => total + w, 0) + GAP * Math.max(0, loaded.length - 1);
  const height = MARGIN * 2 + Math.max(0, ...columnHeights);
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext("2d");
  if (!context) throw new Error("canvas is unavailable");
  context.fillStyle = background;
  context.fillRect(0, 0, width, height);
  context.fillStyle = foreground;
  context.font = "600 13px 'Segoe UI', sans-serif";
  context.textBaseline = "top";
  let x = MARGIN;
  loaded.forEach((board, index) => {
    let y = MARGIN;
    context.fillText(board.label, x, y);
    y += LABEL_HEIGHT + GAP;
    for (const strip of board.strips) {
      context.font = "11px 'Segoe UI', sans-serif";
      context.globalAlpha = 0.7;
      context.fillText(`${strip.line.request.fontFace} · ${strip.line.request.fontSizePt} pt${strip.line.request.bold ? " · bold" : ""}${strip.line.request.italic ? " · italic" : ""}`, x, y);
      context.globalAlpha = 1;
      y += LABEL_HEIGHT;
      context.drawImage(strip.image, x, y);
      y += strip.image.naturalHeight + GAP;
    }
    x += columnWidths[index] + GAP;
  });
  const dataUrl = canvas.toDataURL("image/png");
  return dataUrl.slice(dataUrl.indexOf(",") + 1);
}
