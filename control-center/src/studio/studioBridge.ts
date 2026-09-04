import { emitStudioMessage, subscribeStudioMessage } from "../app/tauri";
import type { StudioDocument } from "./studioModel";

const DOCUMENT_CHANNEL = "studio:document";
const REQUEST_CHANNEL = "studio:request";

/* The main window publishes the Tuner document whenever it changes and
   answers a request from a freshly opened studio; the studio listens.
   Messages are Tauri events across windows, and a local event bus in the
   browser gallery. */
export function publishStudioDocument(document: StudioDocument): void {
  void emitStudioMessage(DOCUMENT_CHANNEL, document).catch(() => undefined);
}

export function answerStudioRequests(current: () => StudioDocument | null): () => void {
  return subscribeStudioMessage<unknown>(REQUEST_CHANNEL, () => {
    const document = current();
    if (document) publishStudioDocument(document);
  });
}

export function requestStudioDocument(): void {
  void emitStudioMessage(REQUEST_CHANNEL, {}).catch(() => undefined);
}

export function subscribeStudioDocument(listener: (document: StudioDocument) => void): () => void {
  return subscribeStudioMessage<StudioDocument>(DOCUMENT_CHANNEL, (document) => {
    if (document && typeof document === "object" && "values" in document) listener(document);
  });
}
