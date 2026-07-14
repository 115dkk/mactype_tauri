import { Trash2 } from "lucide-react";
import type { I18nValue } from "../../i18n/i18n";

export type ListKind = "excludeFonts" | "includeFonts" | "excludeModules" | "includeModules" | "unloadDlls" | "excludeSubstitutionModules";

export interface ListDefinition {
  kind: ListKind;
  label: string;
  help: string;
}

interface ListsEditorProps {
  definitions: ReadonlyArray<ListDefinition>;
  drafts: Readonly<Record<string, string>>;
  fontFamilies: ReadonlyArray<string>;
  installedFontKeys: ReadonlySet<string>;
  fontOptionLabel: (font: string) => string;
  onDraftChange: (kind: ListKind, value: string) => void;
  onCommit: (kind: ListKind) => void;
  onUpdateFontList: (kind: "excludeFonts" | "includeFonts", entries: ReadonlyArray<string>) => void;
  t: I18nValue["t"];
}

export function ListsEditor({ definitions, drafts, fontFamilies, installedFontKeys, fontOptionLabel, onDraftChange, onCommit, onUpdateFontList, t }: ListsEditorProps) {
  return (
    <div className="list-grid">{definitions.map((definition) => {
      if (definition.kind === "excludeFonts" || definition.kind === "includeFonts") {
        const kind: "excludeFonts" | "includeFonts" = definition.kind;
        const entries = (drafts[kind] ?? "").split(/\r?\n/).map((entry) => entry.trim()).filter(Boolean);
        const available = fontFamilies.filter((font) => installedFontKeys.has(font.toLocaleLowerCase()) && !entries.some((entry) => entry.toLocaleLowerCase() === font.toLocaleLowerCase()));
        return (
          <section className="font-list-editor" key={kind}>
            <strong>{definition.label}</strong>
            <span>{definition.help}</span>
            <ul>{entries.map((font) => <li key={font}><span>{fontOptionLabel(font)}</span><button className="icon-button" aria-label={t("profiles.remove", { name: font })} onClick={() => onUpdateFontList(kind, entries.filter((entry) => entry !== font))} type="button"><Trash2 aria-hidden="true" size={14} /></button></li>)}</ul>
            <select aria-label={`${definition.label} · ${t("profiles.addFontToList")}`} disabled={available.length === 0} onChange={(event) => { if (event.target.value) onUpdateFontList(kind, [...entries, event.target.value]); }} value=""><option value="">{t("profiles.addFontToList")}</option>{available.map((font) => <option key={font} value={font}>{font}</option>)}</select>
          </section>
        );
      }
      return <label key={definition.kind}><strong>{definition.label}</strong><span>{definition.help}</span><textarea onBlur={() => onCommit(definition.kind)} onChange={(event) => onDraftChange(definition.kind, event.target.value)} rows={6} value={drafts[definition.kind] ?? ""} /></label>;
    })}</div>
  );
}
