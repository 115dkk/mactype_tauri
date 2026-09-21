import { useCallback, useEffect, useState, type Dispatch, type SetStateAction } from "react";
import type { LegacyProfileCandidate, PreviewRequest, PreviewResult, ProfileEntry, ProfileSnapshot } from "../../app/model";
import { runtime } from "../../app/runtimeAdapter";
import { useProfileDocument, type ProfileNameVerdict } from "../profiles/useProfileDocument";
import { fileName, managedProfileFor, matchesAppliedProfile, sameProfileIdentity } from "../profiles/profileEditorUtils";
import { useI18n } from "../../i18n/i18n";

export const THUMBNAIL_SAMPLE_TEXT = "The quick brown fox jumps over the lazy dog 0123456789";
const THUMBNAIL_WIDTH = 640;
const THUMBNAIL_HEIGHT = 140;
const thumbnailCache = new Map<string, PreviewResult | null>();

export interface FileSettingsModelOptions {
  onEditInTuner?: () => void;
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
export interface FileSettingsProfiles {
  appliedProfile: string | null;
  chooseProfile: (path: string) => Promise<boolean>;
  editInTuner: (path: string) => Promise<void>;
  profiles: readonly ProfileEntry[];
  runProfileAttributes: (entry: ProfileEntry) => { "data-applied": boolean; "data-run-profile": boolean; };
  thumbnails: ReadonlyMap<string, PreviewResult | null>;
}

export interface FileSettingsDocument {
  canDesignate: boolean;
  canSave: boolean;
  designate: () => Promise<void>;
  detailsSummary: string;
  dirtyCount: number;
  encodingText: string;
  offerStart: boolean;
  profile: ProfileSnapshot | null;
  save: () => Promise<void>;
  startServiceNow: () => Promise<void>;
  unsavedText: string;
}

export interface FileSettingsFiles {
  busy: string | null;
  canDuplicate: boolean;
  chooseImport: () => Promise<void>;
  nameDialogOpen: boolean;
  setNameDialogOpen: Dispatch<SetStateAction<boolean>>;
  suggestedProfileName: string;
  profileNameVerdict: (candidate: string) => ProfileNameVerdict;
  documentBusy: boolean;
  duplicate: (name: string) => Promise<boolean>;
  exportIni: () => Promise<void>;
  revealCurrentProfile: () => Promise<void>;
}

export interface FileSettingsLegacy {
  importFrom: (path: string) => Promise<void>;
  legacy: LegacyProfileCandidate | null;
}

export interface FileSettingsMessages {
  error: string | null;
  message: string | null;
}

export interface FileSettingsModel {
  profiles: FileSettingsProfiles;
  document: FileSettingsDocument;
  files: FileSettingsFiles;
  legacy: FileSettingsLegacy;
  messages: FileSettingsMessages;
}

export function useFileSettingsModel({ onEditInTuner }: FileSettingsModelOptions = {}): FileSettingsModel {
  const { t } = useI18n();
  const [nameDialogOpen, setNameDialogOpen] = useState(false);
  const [detectedLegacy, setLegacy] = useState<LegacyProfileCandidate | null>(null);
  const [thumbnails, setThumbnails] = useState<ReadonlyMap<string, PreviewResult | null>>(() => new Map(thumbnailCache));
  const discoverLegacyProfile = useCallback(async () => {
    const detected = await runtime().discoverLegacyProfile();
    setLegacy(detected);
    return detected;
  }, []);
  const {
    appliedProfile,
    chooseProfile,
    command: busy,
    busy: documentBusy,
    recoveryRequired,
    suggestedProfileName,
    profileNameVerdict,
    designateProfile: designate,
    dirtyCount,
    error,
    loading,
    message,
    offerStart,
    profile,
    profiles,
    replaceDocument,
    runFileOperation,
    saveCurrentProfile: save,
    saveProfileAs: duplicate,
    startServiceNow,
  } = useProfileDocument(t, { page: "files", discoverLegacyProfile });
  const legacy = !loading && detectedLegacy && !managedProfileFor(detectedLegacy, profiles) && !sameProfileIdentity(detectedLegacy, appliedProfile)
    ? detectedLegacy
    : null;
  useEffect(() => {
    if (!loading && detectedLegacy && (managedProfileFor(detectedLegacy, profiles) || sameProfileIdentity(detectedLegacy, appliedProfile))) {
      setLegacy(null);
    }
  }, [appliedProfile, detectedLegacy, loading, profiles]);

  useEffect(() => {
    let active = true;
    void (async () => {
      for (const entry of profiles) {
        if (thumbnailCache.has(entry.path)) continue;
        let rendered: PreviewResult | null = null;
        try {
          rendered = await runtime().renderProfilePreview(thumbnailRequest(entry.path));
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

  const editInTuner = async (path: string) => {
    if (await chooseProfile(path)) onEditInTuner?.();
  };

  const importFrom = async (path: string) => {
    await replaceDocument("import", () => runtime().importProfile(path), (opened) => {
      setLegacy(null);
      return t("files.imported", { name: fileName(opened.path) });
    });
  };

  const chooseImport = async () => {
    let selected: string | null = null;
    const picked = await runFileOperation("pick-import", async () => {
      selected = await runtime().pickIniProfile(t("files.iniFilter"));
      return null;
    });
    if (picked && selected) await importFrom(selected);
  };

  const exportIni = async () => {
    if (!profile) return;
    await runFileOperation("export", async () => {
      const selected = await runtime().pickIniExportPath(t("files.iniFilter"), fileName(profile.path));
      if (!selected) return null;
      const destination = await runtime().exportProfile(selected);
      return t("files.exported", { name: fileName(destination) });
    });
  };

  const revealCurrentProfile = async () => {
    await runFileOperation("reveal", async () => {
      const path = await runtime().revealProfileFile();
      return t("files.revealed", { name: fileName(path) });
    });
  };

  const encodingText = profile ? `${profile.encoding.toUpperCase()} · ${profile.lineEnding.replace(/-/g, "").toUpperCase()}` : "—";
  const unsavedText = dirtyCount ? t("files.unsavedCount", { count: dirtyCount }) : t("files.noUnsaved");
  const detailsSummary = profile
    ? `${t("files.fileDetails")} · ${encodingText}${dirtyCount ? ` · ${t("files.unsaved")} ${t("files.unsavedCount", { count: dirtyCount })}` : ""}`
    : `${t("files.fileDetails")} · —`;
  const canSave = Boolean(profile?.canSave) && dirtyCount > 0 && !documentBusy && !recoveryRequired;
  const canDesignate = Boolean(profile) && dirtyCount === 0 && !documentBusy && !recoveryRequired;
  const canDuplicate = Boolean(profile) && !documentBusy && !recoveryRequired;

  return {
    profiles: {
      appliedProfile,
      chooseProfile,
      editInTuner,
      profiles,
      runProfileAttributes: (entry: ProfileEntry) => {
        const applied = matchesAppliedProfile(entry, appliedProfile);
        return { "data-applied": applied, "data-run-profile": applied };
      },
      thumbnails,
    },
    document: {
      canDesignate,
      canSave,
      designate,
      detailsSummary,
      dirtyCount,
      encodingText,
      offerStart,
      profile,
      save,
      startServiceNow,
      unsavedText,
    },
    files: {
      busy,
      canDuplicate,
      chooseImport,
      nameDialogOpen,
      setNameDialogOpen,
      documentBusy,
      suggestedProfileName,
      profileNameVerdict,
      duplicate,
      exportIni,
      revealCurrentProfile,
    },
    legacy: {
      importFrom,
      legacy,
    },
    messages: {
      error,
      message,
    },
  };
}
