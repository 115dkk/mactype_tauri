import { useCallback, useEffect, useState } from "react";
import type { LegacyProfileCandidate, PreviewRequest, PreviewResult, ProfileEntry, ProfileSnapshot } from "../../app/model";
import { operationErrorMessage } from "../../app/operationError";
import {
  applyOpenProfile,
  currentProfile,
  discoverLegacyProfile,
  duplicateProfile,
  exportProfile,
  importProfile,
  listProfiles,
  loadExecutionStatus,
  openProfile,
  pickIniProfile,
  pickIniExportPath,
  renderProfilePreview,
  revealProfileFile,
  saveProfile,
} from "../../app/tauri";
import { openPreferredProfile, rememberProfile } from "../../app/profilePreference";
import { useI18n } from "../../i18n/i18n";

export const THUMBNAIL_SAMPLE_TEXT = "The quick brown fox jumps over the lazy dog 0123456789";
const THUMBNAIL_WIDTH = 640;
const THUMBNAIL_HEIGHT = 140;
const thumbnailCache = new Map<string, PreviewResult | null>();

export interface FileSettingsModelOptions {
  onEditInTuner?: () => void;
}

export function fileName(path: string): string {
  return path.split(/[\\/]/).pop() ?? path;
}

export function matchesAppliedProfile(entry: ProfileEntry, appliedProfile: string | null): boolean {
  if (!appliedProfile) return false;
  const normalized = appliedProfile.toLocaleLowerCase();
  return entry.path.toLocaleLowerCase() === normalized || entry.displayPath.toLocaleLowerCase() === normalized;
}

function sameProfileIdentity(candidate: LegacyProfileCandidate, activeProfile: string | null): boolean {
  if (!activeProfile) return false;
  const stem = (path: string) => fileName(path).replace(/\.ini$/i, "").toLocaleLowerCase();
  return candidate.name.toLocaleLowerCase() === stem(activeProfile) || stem(candidate.path) === stem(activeProfile);
}

function managedProfileFor(candidate: LegacyProfileCandidate, profiles: ReadonlyArray<ProfileEntry>): ProfileEntry | null {
  const candidatePath = candidate.path.toLocaleLowerCase();
  return profiles.find((profile) => profile.path.toLocaleLowerCase() === candidatePath) ?? null;
}

function thumbnailRequest(profilePath: string): PreviewRequest {
  const displayScale = window.devicePixelRatio || 1;
  return {
    profilePath,
    overrides: {},
    displayScale,
    sample: {
      text: THUMBNAIL_SAMPLE_TEXT,
      fontFace: "Segoe UI",
      fontSizePt: 12,
      widthPx: Math.round(THUMBNAIL_WIDTH * displayScale),
      heightPx: Math.round(THUMBNAIL_HEIGHT * displayScale),
      dpi: Math.round(96 * displayScale),
      foreground: "#181D23",
      background: "#EEF1F4",
    },
  };
}

/* Profile file management shared by every skin: the list, the open document
   summary, thumbnails rendered by the helper, and the import/save/apply
   operations with their messages. */
export function useFileSettingsModel({ onEditInTuner }: FileSettingsModelOptions = {}) {
  const { t } = useI18n();
  const [profile, setProfile] = useState<ProfileSnapshot | null>(null);
  const [profiles, setProfiles] = useState<ReadonlyArray<ProfileEntry>>([]);
  const [appliedProfile, setAppliedProfile] = useState<string | null>(null);
  const [legacy, setLegacy] = useState<LegacyProfileCandidate | null>(null);
  const [thumbnails, setThumbnails] = useState<ReadonlyMap<string, PreviewResult | null>>(() => new Map(thumbnailCache));
  const [copyName, setCopyName] = useState("");
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const refreshProfiles = useCallback(async () => {
    setProfiles(await listProfiles());
  }, []);

  useEffect(() => {
    let active = true;
    void Promise.all([currentProfile(), listProfiles(), discoverLegacyProfile(), loadExecutionStatus()])
      .then(async ([opened, available, detected, execution]) => {
        const managedDetected = detected ? managedProfileFor(detected, available) : null;
        const preferredProfile = execution.injectionReady
          ? execution.activeProfile
          : managedDetected?.displayPath ?? execution.activeProfile;
        const selected = await openPreferredProfile(
          opened,
          available,
          preferredProfile,
        );
        if (!active) return;
        setProfile(selected);
        setProfiles(available);
        setAppliedProfile(execution.activeProfile);
        setLegacy(detected && !managedDetected && !sameProfileIdentity(detected, execution.activeProfile) ? detected : null);
      })
      .catch((caught: unknown) => {
        if (active) setError(caught instanceof Error ? caught.message : String(caught));
      });
    return () => {
      active = false;
    };
  }, []);

  useEffect(() => {
    let active = true;
    void (async () => {
      for (const entry of profiles) {
        if (thumbnailCache.has(entry.path)) continue;
        let rendered: PreviewResult | null = null;
        try {
          rendered = await renderProfilePreview(thumbnailRequest(entry.path));
        } catch {
          rendered = null;
        }
        thumbnailCache.set(entry.path, rendered);
        if (!active) return;
        setThumbnails(new Map(thumbnailCache));
      }
    })();
    return () => {
      active = false;
    };
  }, [profiles]);

  const run = async (operation: string, action: () => Promise<ProfileSnapshot>, success: (opened: ProfileSnapshot) => string): Promise<boolean> => {
    setBusy(operation);
    try {
      const opened = await action();
      rememberProfile(opened.path);
      setProfile(opened);
      await refreshProfiles();
      setMessage(success(opened));
      setError(null);
      return true;
    } catch (caught: unknown) {
      setError(operationErrorMessage(caught, t));
      setMessage(null);
      return false;
    } finally {
      setBusy(null);
    }
  };

  const chooseProfile = async (path: string): Promise<boolean> => {
    if (profile?.path === path) return true;
    return run("open", () => openProfile(path), (opened) => t("files.opened", { name: fileName(opened.path) }));
  };

  const editInTuner = async (path: string) => {
    if (await chooseProfile(path)) onEditInTuner?.();
  };

  const duplicate = async () => {
    const name = copyName.trim();
    if (!name) return;
    await run("duplicate", () => duplicateProfile(name), (opened) => {
      setCopyName("");
      return t("files.duplicated", { name: fileName(opened.path) });
    });
  };

  const save = async () => {
    await run("save", async () => {
      const saved = await saveProfile();
      if (!saved) throw new Error(t("profiles.none"));
      return saved;
    }, (opened) => t("files.saved", { name: fileName(opened.path) }));
  };

  const apply = async () => {
    setBusy("apply");
    try {
      const applied = await applyOpenProfile();
      setAppliedProfile(applied.sourceProfile);
      setLegacy((detected) => detected && sameProfileIdentity(detected, applied.sourceProfile) ? null : detected);
      setMessage(t("files.applied", { name: fileName(applied.sourceProfile) }));
      setError(null);
    } catch (caught: unknown) {
      setError(operationErrorMessage(caught, t));
      setMessage(null);
    } finally {
      setBusy(null);
    }
  };

  const importFrom = async (path: string) => {
    await run("import", () => importProfile(path), (opened) => {
      setLegacy(null);
      return t("files.imported", { name: fileName(opened.path) });
    });
  };

  const chooseImport = async () => {
    try {
      const selected = await pickIniProfile(t("files.iniFilter"));
      if (selected) await importFrom(selected);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const exportIni = async () => {
    if (!profile) return;
    setBusy("export");
    try {
      const defaultName = fileName(profile.path);
      const selected = await pickIniExportPath(t("files.iniFilter"), defaultName);
      if (selected) {
        const destination = await exportProfile(selected);
        setMessage(t("files.exported", { name: fileName(destination) }));
        setError(null);
      }
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
      setMessage(null);
    } finally {
      setBusy(null);
    }
  };

  const revealCurrentProfile = async () => {
    setBusy("reveal");
    try {
      const path = await revealProfileFile();
      setMessage(t("files.revealed", { name: fileName(path) }));
      setError(null);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
      setMessage(null);
    } finally {
      setBusy(null);
    }
  };

  const dirtyCount = profile?.dirtyKeys.length ?? 0;
  const encodingText = profile ? `${profile.encoding.toUpperCase()} · ${profile.lineEnding.replace(/-/g, "").toUpperCase()}` : "—";
  const unsavedText = dirtyCount ? t("files.unsavedCount", { count: dirtyCount }) : t("files.noUnsaved");
  const detailsSummary = profile
    ? `${t("files.fileDetails")} · ${encodingText}${dirtyCount ? ` · ${t("files.unsaved")} ${t("files.unsavedCount", { count: dirtyCount })}` : ""}`
    : `${t("files.fileDetails")} · —`;
  const canSave = Boolean(profile?.canSave) && dirtyCount > 0 && busy === null;
  const canApply = Boolean(profile) && dirtyCount === 0 && busy === null;
  const canDuplicate = Boolean(profile) && Boolean(copyName.trim()) && busy === null;

  return {
    apply,
    appliedProfile,
    busy,
    canApply,
    canDuplicate,
    canSave,
    chooseImport,
    chooseProfile,
    copyName,
    detailsSummary,
    dirtyCount,
    duplicate,
    editInTuner,
    encodingText,
    error,
    exportIni,
    importFrom,
    legacy,
    message,
    profile,
    profiles,
    revealCurrentProfile,
    save,
    setCopyName,
    t,
    thumbnails,
    unsavedText,
  };
}

export type FileSettingsModel = ReturnType<typeof useFileSettingsModel>;
