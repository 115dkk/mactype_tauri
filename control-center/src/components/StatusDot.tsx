export type StatusTone = "ok" | "warn" | "bad" | "accent" | "neutral";

/* A filled dot with a soft halo: a status glyph that cannot be misread as a
   character. Skins colour the tones through CSS. */
export function StatusDot({ tone, label }: { tone: StatusTone; label?: string }) {
  return <span aria-hidden={label ? undefined : "true"} aria-label={label} className="status-dot" data-tone={tone} role={label ? "img" : undefined} />;
}
