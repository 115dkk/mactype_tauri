import { useEffect, useState } from "react";
import { loadPreviewFontSubstitutes } from "../../app/tauri";

export function usePreviewFontSubstitutes(path: string | null, revision?: string | null) {
  const [loaded, setLoaded] = useState<{ path: string; mappings: ReadonlyArray<string> } | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    let active = true;
    setError(null);
    if (path) void loadPreviewFontSubstitutes(path).then((mappings) => {
      if (active) setLoaded({ path, mappings });
    }).catch((caught: unknown) => {
      if (active) {
        setLoaded(null);
        setError(caught instanceof Error ? caught.message : String(caught));
      }
    });
    return () => { active = false; };
  }, [path, revision]);
  return { mappings: loaded?.path === path ? loaded.mappings : [], error };
}
