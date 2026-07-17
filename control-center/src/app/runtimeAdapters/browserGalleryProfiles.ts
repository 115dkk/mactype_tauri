import { settingsSchema } from "../../generated/settings";
import type {
  AdvancedProfile,
  IndividualSetting,
  ProfileEntry,
  ProfileSnapshot,
} from "../model";

export const fallbackGalleryProfilePath = "C:\\Program Files\\MacType\\ini\\Default.ini";

const fallbackGalleryProfile: ProfileSnapshot = {
  path: fallbackGalleryProfilePath,
  encoding: "utf-8",
  bom: "none",
  lineEnding: "cr-lf",
  originalHash: "browser-gallery",
  values: Object.fromEntries(settingsSchema.map((setting) => [setting.id, setting.default])),
  dirtyKeys: [],
  canUndo: false,
  canRedo: false,
  individuals: [{ fontFace: "Segoe UI", values: [1, 2, null, null, null, 1] }],
  lists: {
    excludeFonts: ["Raster Fonts"],
    includeFonts: [],
    excludeModules: ["fontview.exe"],
    includeModules: [],
    unloadDlls: [],
    excludeSubstitutionModules: [],
  },
  advanced: {
    shadow: null,
    lcdFilterWeight: null,
    pixelLayout: null,
    displayAffinity: [],
    fontSubstitutes: [],
    infinalityGammaCorrection: [0, 100],
    infinalityFilterParams: [11, 22, 38, 22, 11],
  },
};

const recentGalleryProfile: ProfileSnapshot = {
  ...fallbackGalleryProfile,
  path: "C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\profiles\\Recent.ini",
};

const cloneProfile = (profile: ProfileSnapshot): ProfileSnapshot => structuredClone(profile);

interface BrowserGalleryProfileState {
  current(): ProfileSnapshot;
  discard(): ProfileSnapshot;
  duplicate(name: string): ProfileSnapshot;
  import(path: string): ProfileSnapshot;
  list(): ReadonlyArray<ProfileEntry>;
  open(path: string): ProfileSnapshot;
  openDefault(): ProfileSnapshot;
  redo(): ProfileSnapshot;
  save(): ProfileSnapshot;
  undo(): ProfileSnapshot;
  updateAdvanced(advanced: AdvancedProfile): ProfileSnapshot;
  updateIndividuals(entries: ReadonlyArray<IndividualSetting>): ProfileSnapshot;
  updateList(kind: string, entries: ReadonlyArray<string>): ProfileSnapshot;
  updateSetting(settingId: string, value: number): ProfileSnapshot;
}

export function createBrowserGalleryProfileState(): BrowserGalleryProfileState {
  let profile = cloneProfile(fallbackGalleryProfile);
  let savedProfile = cloneProfile(fallbackGalleryProfile);
  const undoHistory: ProfileSnapshot[] = [];
  const redoHistory: ProfileSnapshot[] = [];

  function open(next: ProfileSnapshot): ProfileSnapshot {
    profile = { ...cloneProfile(next), canUndo: false, canRedo: false };
    savedProfile = cloneProfile(profile);
    undoHistory.length = 0;
    redoHistory.length = 0;
    return cloneProfile(profile);
  }

  function edit(update: (current: ProfileSnapshot) => ProfileSnapshot, dirtyKey: string): ProfileSnapshot {
    undoHistory.push(cloneProfile(profile));
    redoHistory.length = 0;
    const next = update(cloneProfile(profile));
    profile = {
      ...next,
      dirtyKeys: [...new Set([...next.dirtyKeys, dirtyKey])],
      canUndo: true,
      canRedo: false,
    };
    return cloneProfile(profile);
  }

  function moveHistory(from: ProfileSnapshot[], to: ProfileSnapshot[]): ProfileSnapshot {
    const destination = from.pop();
    if (!destination) return cloneProfile(profile);
    to.push(cloneProfile(profile));
    profile = {
      ...cloneProfile(destination),
      canUndo: undoHistory.length > 0,
      canRedo: redoHistory.length > 0,
    };
    return cloneProfile(profile);
  }

  return {
    current: () => cloneProfile(profile),
    discard: () => open(savedProfile),
    duplicate: (name) => open({ ...profile, path: `C:\\Program Files\\MacType\\ini\\${name}.ini` }),
    import: (path) => {
      const fileName = path.split(/[\\/]/).pop() ?? "Imported.ini";
      return open({
        ...fallbackGalleryProfile,
        path: `C:\\Users\\Gallery\\AppData\\Local\\MacType\\ControlCenter\\profiles\\${fileName}`,
      });
    },
    list: () => [
      { name: "Default", path: fallbackGalleryProfile.path },
      { name: "Recent", path: recentGalleryProfile.path },
    ],
    open: (path) => open({ ...fallbackGalleryProfile, path }),
    openDefault: () => open(fallbackGalleryProfile),
    redo: () => moveHistory(redoHistory, undoHistory),
    save: () => {
      profile = { ...profile, dirtyKeys: [], canUndo: false, canRedo: false };
      savedProfile = cloneProfile(profile);
      undoHistory.length = 0;
      redoHistory.length = 0;
      return cloneProfile(profile);
    },
    undo: () => moveHistory(undoHistory, redoHistory),
    updateAdvanced: (advanced) => edit(
      (current) => ({ ...current, advanced: structuredClone(advanced) }),
      "advanced",
    ),
    updateIndividuals: (entries) => edit(
      (current) => ({ ...current, individuals: structuredClone(entries) }),
      "section:Individual",
    ),
    updateList: (kind, entries) => edit(
      (current) => ({ ...current, lists: { ...current.lists, [kind]: [...entries] } }),
      `section:${kind}`,
    ),
    updateSetting: (settingId, value) => edit(
      (current) => ({ ...current, values: { ...current.values, [settingId]: value } }),
      settingId,
    ),
  };
}
