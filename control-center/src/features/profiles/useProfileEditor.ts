import { useEffect, useMemo, useRef, useState, type Dispatch, type SetStateAction, type RefObject } from "react";
import { settingsSchema, type SettingDefinition } from "../../generated/settings";
import { settingMessageKey, useI18n } from "../../i18n/i18n";
import { runtime } from "../../app/runtimeAdapter";
import type { ListDefinition, ListKind } from "./ListsEditor";
import { isSettingDisclosed } from "./boldSubstitution";
import { splitSubstitution } from "./profileEditorUtils";
import type { PreviewVariant, ProfilePreviewHandle } from "./ProfilePreviewPanel";
import { useProfileDocument, type ProfileDocument } from "./useProfileDocument";
import { useStepHistory, type StepHistory } from "./useStepHistory";
import { stepSupportsHistory, guidedStepIds, type GuidedStepId } from "./guidedModel";
import { answerStudioRequests, publishStudioDocument } from "../../studio/studioBridge";
import type { StudioDocument } from "../../studio/studioModel";

export type GroupId = "basic" | "shape" | "lcd" | "advanced" | "individual" | "lists";
export type ProfileMode = "guided" | "all";

/* The guided step is a short column of choices and trades width for height
   readily, so it docks the preview early. The settings table needs room for a
   label beside its control column, so it docks later. Below the threshold the
   preview falls back to a capped bottom panel.

   The advanced figure is what the shipped default window reaches: the workspace
   is the window less the navigation rail, the page padding and the section
   index, so docking by default costs roughly 1300 logical pixels of window.
   Raising it further would push the default past a 1366-wide laptop, and the
   preview only reads as a right column if it starts as one. */
export const DOCKED_PREVIEW_MIN_WIDTH: Readonly<Record<ProfileMode, number>> = { guided: 780, all: 840 };

export interface ProfileEditorOptions {
  mode?: ProfileMode;
}

export interface ProfileEditorGroup {
  id: GroupId;
  label: string;
  description: string;
}

/* The Tuner document, its history, fonts, search, and preview wiring. Every
   skin builds its own chrome around this one hook, so the guided history,
   the docking rule, and the step-aware preview stacks stay identical. */
export interface ProfileEditorEditing {
  activeDefinition: ProfileEditorGroup;
  activeGroup: GroupId;
  activeGuidedLabel: string;
  activeGuidedStep: GuidedStepId;
  changeGuidedSetting: (settingId: string, value: number) => void;
  chooseGroup: (group: GroupId, focusList?: ListKind) => void;
  clearListFocus: () => void;
  filteredSettings: ReadonlyArray<SettingDefinition>;
  groups: readonly ProfileEditorGroup[];
  guidedBusy: boolean;
  headingHint: string;
  headingText: string;
  individualLabels: string[];
  listDefinitions: readonly ListDefinition[];
  listFocus: ListKind | null;
  mode: ProfileMode;
  query: string;
  setActiveGuidedStep: Dispatch<SetStateAction<GuidedStepId>>;
  setQuery: Dispatch<SetStateAction<string>>;
  stepIndex: number;
  guidedStepIds: readonly GuidedStepId[];
}

export interface ProfileEditorHistory {
  redoStepEdit: () => void;
  stepHistory: StepHistory;
  undoStepEdit: () => void;
}

export interface ProfileEditorPreview {
  setPreviewError: Dispatch<SetStateAction<string | null>>;
  fontFace: string;
  fontFamilies: string[];
  fontOptionLabel: (font: string) => string;
  installedFontKeys: Set<string>;
  previewDocked: boolean;
  previewPanelRef: RefObject<ProfilePreviewHandle | null>;
  previewVariants: readonly PreviewVariant[];
  setFontFace: Dispatch<SetStateAction<string>>;
  showPreview: () => void;
  workspaceRef: RefObject<HTMLDivElement | null>;
}

export interface ProfileEditorFiles {
  nameDialogOpen: boolean;
  setNameDialogOpen: Dispatch<SetStateAction<boolean>>;
  submitSaveAs: (name: string) => void;
}

export interface ProfileEditor {
  document: ProfileDocument;
  editing: ProfileEditorEditing;
  history: ProfileEditorHistory;
  preview: ProfileEditorPreview;
  files: ProfileEditorFiles;
}

export function useProfileEditor({ mode = "all" }: ProfileEditorOptions = {}): ProfileEditor {
  const { locale, t } = useI18n();
  const groups = useMemo<ReadonlyArray<ProfileEditorGroup>>(() => [
    { id: "basic", label: t("group.basic.label"), description: t("group.basic.description") },
    { id: "shape", label: t("group.shape.label"), description: t("group.shape.description") },
    { id: "lcd", label: t("group.lcd.label"), description: t("group.lcd.description") },
    { id: "advanced", label: t("group.advanced.label"), description: t("group.advanced.description") },
    { id: "individual", label: t("group.individual.label"), description: t("group.individual.description") },
    { id: "lists", label: t("group.lists.label"), description: t("group.lists.description") },
  ], [t]);
  const individualLabels = useMemo(() => [
    t("individual.hinting"), t("individual.aa"), t("individual.normalWeight"),
    t("individual.boldWeight"), t("individual.slant"), t("individual.kerning"),
  ], [t]);
  const document = useProfileDocument(t);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const {
    advanced,
    busy,
    changeSetting,
    individuals,
    lists,
    profile,
    recoveryRequired,
    values,
  } = document;
  const listDefinitions = useMemo<ReadonlyArray<ListDefinition>>(() => {
    const definitions: ListDefinition[] = [
      { kind: "excludeFonts", label: t("list.excludeFonts.label"), help: t("list.excludeFonts.help") },
      { kind: "includeFonts", label: t("list.includeFonts.label"), help: t("list.includeFonts.help") },
      { kind: "excludeModules", label: t("list.excludeModules.label"), help: t("list.excludeModules.help") },
      { kind: "includeModules", label: t("list.includeModules.label"), help: t("list.includeModules.help") },
      { kind: "unloadDlls", label: t("list.unloadDlls.label"), help: t("list.unloadDlls.help") },
      { kind: "excludeSubstitutionModules", label: t("list.excludeSubstitutionModules.label"), help: t("list.excludeSubstitutionModules.help") },
    ];
    if ((values.unity_font_hook ?? 0) === 1) {
      definitions.push({ kind: "unityIncludeGames", label: t("list.unityIncludeGames.label"), help: t("list.unityIncludeGames.help") });
    } else if ((values.unity_font_hook ?? 0) === 3) {
      definitions.push({ kind: "unityExcludeGames", label: t("list.unityExcludeGames.label"), help: t("list.unityExcludeGames.help") });
    }
    return definitions;
  }, [t, values.unity_font_hook]);
  const [activeGroup, setActiveGroup] = useState<GroupId>("basic");
  /* A list another group sends the reader to; the lists editor scrolls to it
     once and clears it. */
  const [listFocus, setListFocus] = useState<ListKind | null>(null);
  const [activeGuidedStep, setActiveGuidedStep] = useState<GuidedStepId>("start");
  /* Step-scoped guided history. Advanced mode can rewrite the document
     through the global backend history, so the per-step record resets when
     the mode or the open document changes. */
  const stepHistory = useStepHistory(`${mode}::${profile?.path ?? ""}`);
  const [installedFonts, setInstalledFonts] = useState<ReadonlyArray<string>>([]);
  const [fontFace, setFontFace] = useState("Segoe UI");
  const [query, setQuery] = useState("");
  const [nameDialogOpen, setNameDialogOpen] = useState(false);
  const [previewDocked, setPreviewDocked] = useState(false);
  const previewPanelRef = useRef<ProfilePreviewHandle>(null);
  const workspaceRef = useRef<HTMLDivElement>(null);

  const dockedMinimumWidth = DOCKED_PREVIEW_MIN_WIDTH[mode];
  useEffect(() => {
    const workspace = workspaceRef.current;
    if (!workspace) return undefined;
    setPreviewDocked(workspace.clientWidth >= dockedMinimumWidth);
    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) setPreviewDocked(entry.contentRect.width >= dockedMinimumWidth);
    });
    observer.observe(workspace);
    return () => observer.disconnect();
  }, [dockedMinimumWidth]);

  const guidedBusy = !profile || busy || recoveryRequired;
  const changeGuidedSetting = (settingId: string, value: number) => {
    if (stepSupportsHistory(activeGuidedStep)) {
      stepHistory.record(activeGuidedStep, settingId, value, profile?.values[settingId] ?? value);
    }
    changeSetting(settingId, value);
  };
  const undoStepEdit = () => {
    const entry = stepHistory.undo(activeGuidedStep);
    if (entry) changeSetting(entry.settingId, entry.before);
  };
  const redoStepEdit = () => {
    const entry = stepHistory.redo(activeGuidedStep);
    if (entry) changeSetting(entry.settingId, entry.after);
  };

  /* Ctrl+Z / Ctrl+Y inside guided mode drive the step-scoped history. Text
     fields keep their native editing shortcuts, and the default is only
     prevented when this step actually has something to undo or redo. */
  useEffect(() => {
    if (mode !== "guided") return undefined;
    const listener = (event: KeyboardEvent) => {
      if (!event.ctrlKey || event.altKey || event.metaKey) return;
      const target = event.target;
      if (target instanceof HTMLTextAreaElement) return;
      if (target instanceof HTMLInputElement && target.type !== "range" && target.type !== "checkbox" && target.type !== "radio") return;
      const key = event.key.toLocaleLowerCase();
      const wantsUndo = key === "z" && !event.shiftKey;
      const wantsRedo = key === "y" || (key === "z" && event.shiftKey);
      if ((!wantsUndo && !wantsRedo) || guidedBusy) return;
      if (wantsUndo && stepHistory.canUndo(activeGuidedStep)) {
        event.preventDefault();
        undoStepEdit();
      } else if (wantsRedo && stepHistory.canRedo(activeGuidedStep)) {
        event.preventDefault();
        redoStepEdit();
      }
    };
    window.addEventListener("keydown", listener);
    return () => window.removeEventListener("keydown", listener);
  });

  const fontFamilies = useMemo(() => {
    const referenced = [
      fontFace,
      ...individuals.map((entry) => entry.fontFace),
      ...advanced.fontSubstitutes.flatMap((mapping) => {
        const pair = splitSubstitution(mapping);
        return [pair.source, pair.replacement];
      }),
      ...advanced.fontSubstituteBoldPairs.map((pair) => pair.slice(pair.indexOf("=") + 1)),
      ...(lists.excludeFonts ?? []),
      ...(lists.includeFonts ?? []),
    ].map((font) => font.trim()).filter(Boolean);
    const collator = new Intl.Collator(locale, { sensitivity: "base", numeric: true });
    return [...new Set([...installedFonts, ...referenced])]
      .sort((left, right) => collator.compare(left, right));
  }, [advanced.fontSubstituteBoldPairs, advanced.fontSubstitutes, fontFace, individuals, installedFonts, lists.excludeFonts, lists.includeFonts, locale]);
  const installedFontKeys = useMemo(() => new Set(installedFonts.map((font) => font.toLocaleLowerCase())), [installedFonts]);
  const fontOptionLabel = (font: string) => installedFontKeys.has(font.toLocaleLowerCase())
    ? font
    : `${font} · ${t("profiles.fontUnavailable")}`;

  useEffect(() => {
    let active = true;
    void runtime().loadInstalledFontFamilies()
      .then((families) => {
        if (!active) return;
        setInstalledFonts(families);
        setFontFace((current) => families.some((font) => font.toLocaleLowerCase() === current.toLocaleLowerCase()) ? current : families[0] ?? current);
      })
      .catch((error: unknown) => {
        if (active) setPreviewError(error instanceof Error ? error.message : String(error));
      });
    return () => {
      active = false;
    };
  }, [setPreviewError]);

  const filteredSettings = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    return settingsSchema.filter((setting) => {
      if (!isSettingDisclosed(setting.id, values)) return false;
      if (!needle && setting.group !== activeGroup) return false;
      const localized = `${t(settingMessageKey(setting.id, "label"))} ${t(settingMessageKey(setting.id, "description"))} ${setting.key}`;
      return !needle || localized.toLocaleLowerCase().includes(needle);
    });
  }, [activeGroup, query, t, values]);

  const showPreview = () => {
    previewPanelRef.current?.show();
  };

  /* The Preview Studio window follows this document: every change is
     published, and a studio that opens later asks for the current one. */
  const studioDocument = useMemo<StudioDocument | null>(() => profile ? {
    profilePath: profile.path,
    profileName: profile.displayPath.split(/[/\\]/).pop() ?? profile.displayPath,
    values: { ...values },
    savedValues: { ...(profile.savedValues ?? {}) },
    fontFace,
  } : null, [fontFace, profile, values]);
  const studioDocumentRef = useRef(studioDocument);
  useEffect(() => {
    studioDocumentRef.current = studioDocument;
    if (studioDocument) publishStudioDocument(studioDocument);
  }, [studioDocument]);
  useEffect(() => answerStudioRequests(() => studioDocumentRef.current), []);

  /* Step-aware preview stacks, mirroring the legacy Tuner screens: the bold
     and italic screen compares the three styles and the LCD screen compares
     the current method against the red, green, and blue channels
     (channel-pure foregrounds isolate each subpixel). Every other screen
     renders the sample once, because a second sample group would claim the
     height the step body needs. */
  const previewVariants = useMemo<ReadonlyArray<PreviewVariant>>(() => {
    const pangram = t("profiles.samplePangram");
    if (mode === "guided" && activeGuidedStep === "boldItalic") {
      return [
        { key: "bold", label: t("guided.previewBold"), text: pangram, bold: true },
        { key: "italic", label: t("guided.previewItalic"), text: pangram, italic: true },
        { key: "bold-italic", label: t("guided.previewBoldItalic"), text: pangram, bold: true, italic: true },
      ];
    }
    if (mode === "guided" && activeGuidedStep === "lcd") {
      return [
        { key: "current", label: t("guided.previewCurrent"), text: pangram },
        { key: "channel-r", label: "R", text: pangram, foreground: "#C80000" },
        { key: "channel-g", label: "G", text: pangram, foreground: "#008A00" },
        { key: "channel-b", label: "B", text: pangram, foreground: "#0000C8" },
      ];
    }
    return [{ key: "normal", label: null }];
  }, [activeGuidedStep, mode, t]);

  const activeDefinition = groups.find((group) => group.id === activeGroup) ?? groups[0];
  const activeGuidedLabel = t(`guided.${activeGuidedStep}`);
  const stepIndex = guidedStepIds.indexOf(activeGuidedStep);
  const chooseGroup = (group: GroupId, focusList?: ListKind) => {
    setActiveGroup(group);
    setQuery("");
    setListFocus(focusList ?? null);
  };
  const clearListFocus = () => setListFocus(null);
  const headingText = mode === "guided" ? activeGuidedLabel : query ? t("profiles.searchResults") : activeDefinition.label;
  const headingHint = mode === "guided" ? t("guided.guidance") : query ? t("profiles.searchDescription", { query }) : activeDefinition.description;
  const submitSaveAs = (name: string) => {
    void document.saveProfileAs(name).then((saved) => {
      if (saved) setNameDialogOpen(false);
    });
  };

  return {
    document: { ...document, error: document.error ?? previewError },
    editing: {
      activeDefinition,
      activeGroup,
      activeGuidedLabel,
      activeGuidedStep,
      changeGuidedSetting,
      chooseGroup,
      clearListFocus,
      filteredSettings,
      groups,
      guidedBusy,
      headingHint,
      headingText,
      individualLabels,
      listDefinitions,
      listFocus,
      mode,
      query,
      setActiveGuidedStep,
      setQuery,
      stepIndex,
      guidedStepIds,
    },
    history: {
      redoStepEdit,
      stepHistory,
      undoStepEdit,
    },
    preview: {
      setPreviewError,
      fontFace,
      fontFamilies,
      fontOptionLabel,
      installedFontKeys,
      previewDocked,
      previewPanelRef,
      previewVariants,
      setFontFace,
      showPreview,
      workspaceRef,
    },
    files: {
      nameDialogOpen,
      setNameDialogOpen,
      submitSaveAs,
    },
  };
}
