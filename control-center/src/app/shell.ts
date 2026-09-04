import { Activity, FileCog, Home, ServerCog, SlidersHorizontal, Sparkles, type LucideIcon } from "lucide-react";
import type { InstallationStatus, ViewId } from "./model";
import type { SkinPreference } from "./skinPreference";
import type { ThemePreference } from "./themePreference";
import type { MessageKey } from "../i18n/i18n";

export type ProfileMode = "quick" | "advanced";

/* Everything a skin shell needs from the application: where we are, how to
   move, the preferences, the installation findings, and the callbacks the
   pages report through. Shells never own this state; they arrange it. */
export interface ShellProps {
  view: ViewId;
  profileMode: ProfileMode;
  navigate: (view: ViewId, profileMode?: ProfileMode) => void;
  theme: ThemePreference;
  toggleTheme: () => void;
  skin: SkinPreference;
  setSkin: (skin: SkinPreference) => void;
  status: InstallationStatus;
  setStatus: (status: InstallationStatus) => void;
  ciSmoke: boolean;
  reportReady: (view: ViewId) => void;
  reconnectPreview: () => Promise<InstallationStatus>;
  rediscoverInstallation: () => Promise<InstallationStatus>;
  openPreviewStudio: () => void;
}

export type NavId = "overview" | "files" | "execution" | "guided" | "all" | "diagnostics";

export interface NavEntry {
  id: NavId;
  view: ViewId;
  profileMode?: ProfileMode;
  labelKey: MessageKey;
  icon: LucideIcon;
  /* Group key for skins that draw section headings (wizard, tuner, tools). */
  group: "wizardGroup" | "tunerGroup" | "toolsGroup" | null;
}

/* The navigation model every skin renders: the same six entries in the same
   order, grouped the same way, so the reader finds the same view in the same
   place after switching skins. */
export const navEntries: ReadonlyArray<NavEntry> = [
  { id: "overview", view: "overview", labelKey: "nav.overview", icon: Home, group: null },
  { id: "files", view: "files", labelKey: "nav.profiles", icon: FileCog, group: "wizardGroup" },
  { id: "execution", view: "execution", labelKey: "nav.execution", icon: ServerCog, group: "wizardGroup" },
  { id: "guided", view: "profiles", profileMode: "quick", labelKey: "nav.guidedSetup", icon: Sparkles, group: "tunerGroup" },
  { id: "all", view: "profiles", profileMode: "advanced", labelKey: "nav.allSettings", icon: SlidersHorizontal, group: "tunerGroup" },
  { id: "diagnostics", view: "diagnostics", labelKey: "nav.diagnostics", icon: Activity, group: "toolsGroup" },
];

export function navGroupLabelKey(group: NonNullable<NavEntry["group"]>): MessageKey {
  return `nav.${group}` as MessageKey;
}

export function isNavSelected(entry: NavEntry, view: ViewId, profileMode: ProfileMode): boolean {
  if (entry.view !== view) return false;
  return entry.profileMode === undefined || entry.profileMode === profileMode;
}

export function activeNavEntry(view: ViewId, profileMode: ProfileMode): NavEntry {
  return navEntries.find((entry) => isNavSelected(entry, view, profileMode)) ?? navEntries[0];
}

export function viewLabelKey(view: ViewId, profileMode: ProfileMode): MessageKey {
  return activeNavEntry(view, profileMode).labelKey;
}
