import { useEffect, useState } from "react";
import { loadPreviewFontSubstitutes } from "../../app/tauri";

interface LoadedSubstitutes {
  path: string;
  revision: string | null;
  mappings: ReadonlyArray<string>;
}

/* The last answer per profile and revision, kept across mounts so a page
   that comes back knows its substituted family before its first paint. */
const substituteCache = new Map<string, LoadedSubstitutes>();
const SUBSTITUTE_CACHE_LIMIT = 16;

const cacheKey = (path: string, revision: string | null) => `${path}\u0000${revision ?? ""}`;

export function usePreviewFontSubstitutes(path: string | null, revision?: string | null) {
  const key = path ? cacheKey(path, revision ?? null) : null;
  const [loaded, setLoaded] = useState<LoadedSubstitutes | null>(() => (key ? substituteCache.get(key) ?? null : null));
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    let active = true;
    setError(null);
    if (path && key && !substituteCache.has(key)) void loadPreviewFontSubstitutes(path).then((mappings) => {
      const entry = { path, revision: revision ?? null, mappings };
      substituteCache.delete(key);
      substituteCache.set(key, entry);
      while (substituteCache.size > SUBSTITUTE_CACHE_LIMIT) {
        const oldest = substituteCache.keys().next().value;
        if (oldest === undefined) break;
        substituteCache.delete(oldest);
      }
      if (active) setLoaded(entry);
    }).catch((caught: unknown) => {
      if (active) {
        setLoaded(null);
        setError(caught instanceof Error ? caught.message : String(caught));
      }
    });
    return () => { active = false; };
  }, [key, path, revision]);
  const current = key ? substituteCache.get(key) ?? (loaded && cacheKey(loaded.path, loaded.revision) === key ? loaded : null) : null;
  /* Ready once the answer for this profile is known, or there is no profile,
     or the request failed; callers wait for it before rendering a specimen. */
  return { mappings: current?.mappings ?? [], error, ready: !path || current !== null || error !== null };
}
