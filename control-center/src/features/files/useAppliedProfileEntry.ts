import { useEffect, useState } from "react";
import type { ProfileEntry } from "../../app/model";
import { listProfiles } from "../../app/tauri";
import { matchesAppliedProfile } from "./useFileSettingsModel";

/* Resolves the applied profile (as the service names it) to a profile entry
   with an on-disk path the helper can render. */
export function useAppliedProfileEntry(activeProfile: string | null): ProfileEntry | null {
  const [profiles, setProfiles] = useState<ReadonlyArray<ProfileEntry>>([]);
  useEffect(() => {
    let active = true;
    void listProfiles().then((entries) => {
      if (active) setProfiles(entries);
    }).catch(() => undefined);
    return () => { active = false; };
  }, []);
  return profiles.find((entry) => matchesAppliedProfile(entry, activeProfile)) ?? null;
}
