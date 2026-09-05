import { useEffect, useMemo, useRef, useState } from "react";
import { settingsSchema } from "../../generated/settings";
import { settingMessageKey, useI18n } from "../../i18n/i18n";
import { loadInstalledFontFamilies } from "../../app/tauri";
import type { ListDefinition } from "../../pages/profiles/ListsEditor";
import { splitSubstitution } from "../../pages/profiles/profileEditorUtils";
import type { PreviewVariant, ProfilePreviewHandle } from "../../pages/profiles/ProfilePreviewPanel";
import { useProfileDocument } from "../../pages/profiles/useProfileDocument";
import { useStepHistory } from "../../pages/profiles/useStepHistory";
import { stepSupportsHistory, wizardStepIds, type WizardStepId } from "../../pages/profiles/wizardModel";
import { answerStudioRequests, publishStudioDocument } from "../../studio/studioBridge";
import type { StudioDocument } from "../../studio/studioModel";

export type GroupId = "basic" | "shape" | "lcd" | "advanced" | "individual" | "lists";
export type ProfileMode = "quick" | "advanced";

/* The guided step is a short column of choices and trades width for height
   readily, so it docks the preview early. The settings table needs room for a
   label beside its control column, so it docks later. Below the threshold the
   preview falls back to a capped bottom panel.

   The advanced figure is what the shipped default window reaches: the workspace
   is the window less the navigation rail, the page padding and the section
   index, so docking by default costs roughly 1300 logical pixels of window.
   Raising it further would push the default past a 1366-wide laptop, and the
   preview only reads as a right column if it starts as one. */
export const DOCKED_PREVIEW_MIN_WIDTH: Readonly<Record<ProfileMode, number>> = { quick: 780, advanced: 840 };

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
export function useProfileEditor({ mode = "advanced" }: ProfileEditorOptions = {}) {
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
    dirtyCount,
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
  const [activeWizardStep, setActiveWizardStep] = useState<WizardStepId>("start");
  /* Step-scoped guided history. Advanced mode can rewrite the document
     through the global backend history, so the per-step record resets when
     the mode or the open document changes. */
  const stepHistory = useStepHistory(`${mode}::${profile?.path ?? ""}`);
  const [installedFonts, setInstalledFonts] = useState<ReadonlyArray<string>>([]);
  const [fontFace, setFontFace] = useState("Segoe UI");
  const [query, setQuery] = useState("");
  const [saveAsOpen, setSaveAsOpen] = useState(false);
  const [saveAsName, setSaveAsName] = useState("");
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
    if (stepSupportsHistory(activeWizardStep)) {
      stepHistory.record(activeWizardStep, settingId, value, profile?.values[settingId] ?? value);
    }
    changeSetting(settingId, value);
  };
  const undoStepEdit = () => {
    const entry = stepHistory.undo(activeWizardStep);
    if (entry) changeSetting(entry.settingId, entry.before);
  };
  const redoStepEdit = () => {
    const entry = stepHistory.redo(activeWizardStep);
    if (entry) changeSetting(entry.settingId, entry.after);
  };

  /* Ctrl+Z / Ctrl+Y inside guided mode drive the step-scoped history. Text
     fields keep their native editing shortcuts, and the default is only
     prevented when this step actually has something to undo or redo. */
  useEffect(() => {
    if (mode !== "quick") return undefined;
    const listener = (event: KeyboardEvent) => {
      if (!event.ctrlKey || event.altKey || event.metaKey) return;
      const target = event.target;
      if (target instanceof HTMLTextAreaElement) return;
      if (target instanceof HTMLInputElement && target.type !== "range" && target.type !== "checkbox" && target.type !== "radio") return;
      const key = event.key.toLocaleLowerCase();
      const wantsUndo = key === "z" && !event.shiftKey;
      const wantsRedo = key === "y" || (key === "z" && event.shiftKey);
      if ((!wantsUndo && !wantsRedo) || guidedBusy) return;
      if (wantsUndo && stepHistory.canUndo(activeWizardStep)) {
        event.preventDefault();
        undoStepEdit();
      } else if (wantsRedo && stepHistory.canRedo(activeWizardStep)) {
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
      ...(lists.excludeFonts ?? []),
      ...(lists.includeFonts ?? []),
    ].map((font) => font.trim()).filter(Boolean);
    const collator = new Intl.Collator(locale, { sensitivity: "base", numeric: true });
    return [...new Set([...installedFonts, ...referenced])]
      .sort((left, right) => collator.compare(left, right));
  }, [advanced.fontSubstitutes, fontFace, individuals, installedFonts, lists.excludeFonts, lists.includeFonts, locale]);
  const installedFontKeys = useMemo(() => new Set(installedFonts.map((font) => font.toLocaleLowerCase())), [installedFonts]);
  const fontOptionLabel = (font: string) => installedFontKeys.has(font.toLocaleLowerCase())
    ? font
    : `${font} · ${t("profiles.fontUnavailable")}`;

  useEffect(() => {
    let active = true;
    void loadInstalledFontFamilies()
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
      if (!needle && setting.group !== activeGroup) return false;
      const localized = `${t(settingMessageKey(setting.id, "label"))} ${t(settingMessageKey(setting.id, "description"))} ${setting.key}`;
      return !needle || localized.toLocaleLowerCase().includes(needle);
    });
  }, [activeGroup, query, t]);

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
    if (mode === "quick" && activeWizardStep === "boldItalic") {
      return [
        { key: "bold", label: t("wizard.previewBold"), text: pangram, bold: true },
        { key: "italic", label: t("wizard.previewItalic"), text: pangram, italic: true },
        { key: "bold-italic", label: t("wizard.previewBoldItalic"), text: pangram, bold: true, italic: true },
      ];
    }
    if (mode === "quick" && activeWizardStep === "lcd") {
      return [
        { key: "current", label: t("wizard.previewCurrent"), text: pangram },
        { key: "channel-r", label: "R", text: pangram, foreground: "#C80000" },
        { key: "channel-g", label: "G", text: pangram, foreground: "#008A00" },
        { key: "channel-b", label: "B", text: pangram, foreground: "#0000C8" },
      ];
    }
    return [{ key: "normal", label: null }];
  }, [activeWizardStep, mode, t]);

  const activeDefinition = groups.find((group) => group.id === activeGroup) ?? groups[0];
  const activeWizardLabel = t(`wizard.${activeWizardStep}`);
  const stepIndex = wizardStepIds.indexOf(activeWizardStep);
  const chooseGroup = (group: GroupId) => {
    setActiveGroup(group);
    setQuery("");
  };
  const headingText = mode === "quick" ? activeWizardLabel : query ? t("profiles.searchResults") : activeDefinition.label;
  const headingHint = mode === "quick" ? t("wizard.guidance") : query ? t("profiles.searchDescription", { query }) : activeDefinition.description;
  const submitSaveAs = () => {
    void document.saveProfileAs(saveAsName).then((saved) => {
      if (saved) {
        setSaveAsName("");
        setSaveAsOpen(false);
      }
    });
  };

  return {
    ...document,
    error: document.error ?? previewError,
    setPreviewError,
    activeDefinition,
    activeGroup,
    activeWizardLabel,
    activeWizardStep,
    changeGuidedSetting,
    chooseGroup,
    dirtyCount,
    filteredSettings,
    fontFace,
    fontFamilies,
    fontOptionLabel,
    groups,
    guidedBusy,
    headingHint,
    headingText,
    individualLabels,
    installedFontKeys,
    listDefinitions,
    locale,
    mode,
    previewDocked,
    previewPanelRef,
    previewVariants,
    query,
    redoStepEdit,
    saveAsName,
    saveAsOpen,
    setActiveWizardStep,
    setFontFace,
    setQuery,
    setSaveAsName,
    setSaveAsOpen,
    showPreview,
    stepHistory,
    stepIndex,
    submitSaveAs,
    t,
    undoStepEdit,
    wizardStepIds,
    workspaceRef,
  };
}

export type ProfileEditor = ReturnType<typeof useProfileEditor>;
