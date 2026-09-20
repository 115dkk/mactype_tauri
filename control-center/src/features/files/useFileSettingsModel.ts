import { useCallback, useEffect, useState } from "react";
import type { DesignationEffect, ExecutionStatus, LegacyProfileCandidate, PreviewRequest, PreviewResult, ProfileEntry, ProfileSnapshot } from "../../app/model";
import { operationErrorMessage } from "../../app/operationError";
import {
  currentProfile,
  designateOpenProfile,
  discoverLegacyProfile,
  duplicateProfile,
  exportProfile,
  importProfile,
  listProfiles,
  loadExecutionStatus,
  manageSystemService,
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
   summary, thumbnails rendered by the helper, and the import/save/designate
   operations with their messages. Designating sets the run profile without
   turning the service on; the one start this model offers is the separate,
   labelled start-now action after a designation a stopped service holds. */
export function useFileSettingsModel({ onEditInTuner }: FileSettingsModelOptions = {}) {
  const { t } = useI18n();
  const [profile, setProfile] = useState<ProfileSnapshot | null>(null);
  const [profiles, setProfiles] = useState<ReadonlyArray<ProfileEntry>>([]);
  const [appliedProfile, setAppliedProfile] = useState<string | null>(null);
  const [execution, setExecution] = useState<ExecutionStatus | null>(null);
  const [designationEffect, setDesignationEffect] = useState<DesignationEffect | null>(null);
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
        setExecution(execution);
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
    setDesignationEffect(null);
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

  const designate = async () => {
    setBusy("designate");
    setDesignationEffect(null);
    try {
      const designated = await designateOpenProfile();
      const name = fileName(designated.sourceProfile);
      setAppliedProfile(designated.sourceProfile);
      setLegacy((detected) => detected && sameProfileIdentity(detected, designated.sourceProfile) ? null : detected);
      setMessage(t(designated.effect === "live" ? "profiles.designatedLive" : "profiles.designatedNextStart", { name }));
      setDesignationEffect(designated.effect);
      setError(null);
      setExecution(await loadExecutionStatus());
    } catch (caught: unknown) {
      setError(operationErrorMessage(caught, t));
      setMessage(null);
    } finally {
      setBusy(null);
    }
  };

  // The one place this model may turn the service on: a separate, labelled
  // click offered after a designation that the stopped service is holding.
  const startServiceNow = async () => {
    setBusy("start");
    setDesignationEffect(null);
    try {
      const next = await manageSystemService("start");
      setExecution(next);
      setAppliedProfile(next.activeProfile);
      setMessage(t("files.serviceStartedWithRunProfile", { name: next.activeProfile ? fileName(next.activeProfile) : "" }));
      setError(null);
    } catch (caught: unknown) {
      setError(operationErrorMessage(caught, t, "execution.operationFailed"));
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
    setDesignationEffect(null);
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
    setDesignationEffect(null);
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
  const canDesignate = Boolean(profile) && dirtyCount === 0 && busy === null;
  const serviceCanStart = Boolean(
    execution?.systemService.installation === "current"
      && execution.systemService.runtime === "stopped"
      && execution.systemService.canStart,
  );
  const offerStart = designationEffect === "next-start" && serviceCanStart;
  const canDuplicate = Boolean(profile) && Boolean(copyName.trim()) && busy === null;

  return {
    appliedProfile,
    busy,
    canDesignate,
    canDuplicate,
    canSave,
    chooseImport,
    chooseProfile,
    copyName,
    designate,
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
    offerStart,
    profile,
    profiles,
    revealCurrentProfile,
    runProfileAttributes: (entry: ProfileEntry) => {
      const applied = matchesAppliedProfile(entry, appliedProfile);
      return { "data-applied": applied, "data-run-profile": applied };
    },
    save,
    setCopyName,
    startServiceNow,
    t,
    thumbnails,
    unsavedText,
  };
}

export type FileSettingsModel = ReturnType<typeof useFileSettingsModel>;
