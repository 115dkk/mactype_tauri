import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { AdvancedProfile, IndividualSetting, ProfileSnapshot } from "../../app/model";
import { operationErrorMessage } from "../../app/operationError";
import { openPreferredProfile, rememberProfile } from "../../app/profilePreference";
import {
  applyOpenProfile,
  currentProfile,
  discardProfileChanges,
  duplicateProfile,
  listProfiles,
  loadExecutionStatus,
  redoProfile,
  resetProfileDefaults,
  saveProfile,
  undoProfile,
  updateProfileAdvanced,
  updateProfileIndividuals,
  updateProfileList,
  updateProfileSetting,
} from "../../app/tauri";
import { settingsSchema } from "../../generated/settings";
import type { I18nValue } from "../../i18n/i18n";

type ProfileCommand = "undo" | "redo" | "discard" | "save" | "save-as" | "apply";

const emptyAdvancedProfile: AdvancedProfile = {
  shadow: null,
  lcdFilterWeight: null,
  pixelLayout: null,
  fontSubstitutes: [],
};

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function cloneAdvancedProfile(advanced: AdvancedProfile): AdvancedProfile {
  return {
    ...advanced,
    shadow: advanced.shadow ? { ...advanced.shadow } : null,
    lcdFilterWeight: advanced.lcdFilterWeight ? [...advanced.lcdFilterWeight] : null,
    pixelLayout: advanced.pixelLayout ? [...advanced.pixelLayout] : null,
    fontSubstitutes: [...advanced.fontSubstitutes],
  };
}

function profileListDrafts(profile: ProfileSnapshot): Record<string, string> {
  return {
    excludeFonts: profile.lists.excludeFonts.join("\n"),
    includeFonts: profile.lists.includeFonts.join("\n"),
    excludeModules: profile.lists.excludeModules.join("\n"),
    includeModules: profile.lists.includeModules.join("\n"),
    unloadDlls: profile.lists.unloadDlls.join("\n"),
    excludeSubstitutionModules: profile.lists.excludeSubstitutionModules.join("\n"),
  };
}

export function useProfileDocument(t: I18nValue["t"]) {
  const [profile, setProfile] = useState<ProfileSnapshot | null>(null);
  const [values, setValues] = useState<Record<string, number>>(
    Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])),
  );
  const [individuals, setIndividuals] = useState<IndividualSetting[]>([]);
  const [listDrafts, setListDrafts] = useState<Record<string, string>>({});
  const [advanced, setAdvanced] = useState<AdvancedProfile>(emptyAdvancedProfile);
  const [loading, setLoading] = useState(true);
  const [pendingEdits, setPendingEdits] = useState(0);
  const [command, setCommand] = useState<ProfileCommand | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [recoveryRequired, setRecoveryRequired] = useState(false);
  const mutationQueue = useRef<Promise<void>>(Promise.resolve());

  const applySnapshot = useCallback((opened: ProfileSnapshot) => {
    rememberProfile(opened.path);
    setProfile(opened);
    setValues(opened.values);
    setIndividuals(opened.individuals.map((entry) => ({ ...entry, values: [...entry.values] })));
    setListDrafts(profileListDrafts(opened));
    setAdvanced(cloneAdvancedProfile(opened.advanced));
  }, []);

  const queueMutation = useCallback((mutation: () => Promise<ProfileSnapshot | null>) => {
    setPendingEdits((current) => current + 1);
    const operation = mutationQueue.current.then(mutation);
    mutationQueue.current = operation.then(() => undefined, () => undefined);
    void operation
      .then((snapshot) => {
        if (snapshot) setProfile(snapshot);
        setMessage(null);
      })
      .catch((caught: unknown) => {
        setRecoveryRequired(true);
        setError(errorMessage(caught));
      })
      .finally(() => setPendingEdits((current) => Math.max(0, current - 1)));
  }, []);

  useEffect(() => {
    let active = true;
    void Promise.all([currentProfile(), listProfiles(), loadExecutionStatus()])
      .then(([current, available, execution]) => openPreferredProfile(current, available, execution.activeProfile))
      .then((opened) => {
        if (active && opened) applySnapshot(opened);
      })
      .catch((caught: unknown) => {
        if (active) setError(errorMessage(caught));
      })
      .finally(() => {
        if (active) setLoading(false);
      });
    return () => {
      active = false;
    };
  }, [applySnapshot]);

  const previewSetting = (settingId: string, value: number) => {
    setValues((current) => ({ ...current, [settingId]: value }));
  };

  const changeSetting = (settingId: string, value: number) => {
    previewSetting(settingId, value);
    queueMutation(() => updateProfileSetting(settingId, value));
  };

  const commitIndividuals = (next: IndividualSetting[]) => {
    setIndividuals(next);
    queueMutation(() => updateProfileIndividuals(next));
  };

  const addIndividual = (font: string) => {
    const normalized = font.trim();
    if (!normalized || individuals.some((entry) => entry.fontFace.toLocaleLowerCase() === normalized.toLocaleLowerCase())) return;
    commitIndividuals([...individuals, { fontFace: normalized, values: [null, null, null, null, null, null] }]);
  };

  const updateListDraft = (kind: string, value: string) => {
    setListDrafts((current) => ({ ...current, [kind]: value }));
  };

  const commitList = (kind: string) => {
    const entries = (listDrafts[kind] ?? "").split(/\r?\n/).map((entry) => entry.trim()).filter(Boolean);
    queueMutation(() => updateProfileList(kind, entries));
  };

  const commitAdvanced = (next: AdvancedProfile) => {
    setAdvanced(next);
    queueMutation(() => updateProfileAdvanced(next));
  };

  const resetDefaults = () => {
    setValues(Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])));
    queueMutation(() => resetProfileDefaults());
  };

  const updateFontList = (kind: "excludeFonts" | "includeFonts", entries: ReadonlyArray<string>) => {
    updateListDraft(kind, entries.join("\n"));
    queueMutation(() => updateProfileList(kind, entries));
  };

  const runHistoryCommand = async (nextCommand: "undo" | "redo" | "discard") => {
    const action = nextCommand === "undo" ? undoProfile : nextCommand === "redo" ? redoProfile : discardProfileChanges;
    setCommand(nextCommand);
    try {
      await mutationQueue.current;
      applySnapshot(await action());
      setRecoveryRequired(false);
      setMessage(null);
      setError(null);
    } catch (caught: unknown) {
      setError(errorMessage(caught));
    } finally {
      setCommand(null);
    }
  };

  const applyProfile = async () => {
    if (recoveryRequired || (profile?.dirtyKeys.length ?? 0) > 0) return;
    setCommand("apply");
    try {
      await mutationQueue.current;
      const applied = await applyOpenProfile();
      const name = applied.sourceProfile.split(/[\\/]/).pop() ?? applied.sourceProfile;
      setMessage(t("profiles.applied", { name }));
      setError(null);
    } catch (caught: unknown) {
      setError(operationErrorMessage(caught, t));
      setMessage(null);
    } finally {
      setCommand(null);
    }
  };

  const saveProfileAs = async (name: string) => {
    if (recoveryRequired || !name.trim()) return false;
    setCommand("save-as");
    try {
      await mutationQueue.current;
      const saved = await duplicateProfile(name.trim());
      applySnapshot(saved);
      setMessage(t("profiles.savedAs", { path: saved.displayPath }));
      setError(null);
      return true;
    } catch (caught: unknown) {
      setError(errorMessage(caught));
      setMessage(null);
      return false;
    } finally {
      setCommand(null);
    }
  };

  const saveCurrentProfile = async () => {
    if (recoveryRequired) return;
    setCommand("save");
    try {
      await mutationQueue.current;
      const saved = await saveProfile();
      if (!saved) throw new Error(t("profiles.none"));
      applySnapshot(saved);
      const name = saved.path.split(/[\\/]/).pop() ?? saved.path;
      setMessage(t("profiles.savedNow", { name }));
      setError(null);
    } catch (caught: unknown) {
      setError(errorMessage(caught));
      setMessage(null);
    } finally {
      setCommand(null);
    }
  };

  const dirtyKeys = useMemo(() => {
    const keys = new Set(profile?.dirtyKeys ?? []);
    if (profile) {
      for (const setting of settingsSchema) {
        if (values[setting.id] !== profile.values[setting.id]) keys.add(setting.id);
      }
    }
    return [...keys];
  }, [profile, values]);
  const busy = pendingEdits > 0 || command !== null;
  const dirtyCount = dirtyKeys.length;

  return {
    addIndividual,
    advanced,
    applyProfile,
    busy,
    changeSetting,
    command,
    commitAdvanced,
    commitIndividuals,
    commitList,
    dirtyCount,
    dirtyKeys,
    discard: () => runHistoryCommand("discard"),
    error,
    individuals,
    listDrafts,
    loading,
    message,
    previewSetting,
    profile,
    recoveryRequired,
    redo: () => runHistoryCommand("redo"),
    resetDefaults,
    savedValues: profile?.savedValues,
    saveCurrentProfile,
    saveProfileAs,
    setAdvanced,
    setError,
    undo: () => runHistoryCommand("undo"),
    updateFontList,
    updateListDraft,
    values,
  };
}
