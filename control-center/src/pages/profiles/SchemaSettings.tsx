import { RotateCcw } from "lucide-react";
import { useRef, useState } from "react";
import type { SettingDefinition } from "../../generated/settings";
import {
  settingMessageKey,
  settingOptionMessageKey,
  type I18nValue,
} from "../../i18n/i18n";

interface SchemaSettingsProps {
  settings: ReadonlyArray<SettingDefinition>;
  values: Readonly<Record<string, number>>;
  dirtyKeys: ReadonlyArray<string>;
  onChange: (settingId: string, value: number) => void;
  onPreviewChange: (settingId: string, value: number) => void;
  t: I18nValue["t"];
}

interface SettingControlProps {
  setting: SettingDefinition;
  settingLabel: string;
  value: number;
  onCommit: (value: number) => void;
  onPreview: (value: number) => void;
  t: I18nValue["t"];
}

const rangeAdjustmentKeys = new Set(["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "PageUp", "PageDown", "Home", "End"]);

function SettingControl({ setting, settingLabel, value, onCommit, onPreview, t }: SettingControlProps) {
  const rangeStart = useRef<number | null>(null);
  const numberStart = useRef<number | null>(null);
  const [numberDraft, setNumberDraft] = useState<string | null>(null);
  const step = setting.type === "integer" ? 1 : 0.01;
  const normalize = (next: number) => Math.min(setting.max, Math.max(setting.min, setting.type === "integer" ? Math.round(next) : next));
  const parseNumber = (raw: string) => {
    if (raw.trim() === "") return null;
    const parsed = Number(raw);
    return Number.isFinite(parsed) ? parsed : null;
  };
  const beginRange = () => {
    if (rangeStart.current === null) rangeStart.current = value;
  };
  const finishRange = (next: number) => {
    const start = rangeStart.current;
    rangeStart.current = null;
    if (start !== null && next !== start) onCommit(next);
  };
  const previewRange = (next: number) => {
    onPreview(next);
    if (rangeStart.current === null) onCommit(next);
  };
  const beginNumber = () => {
    if (numberStart.current === null) numberStart.current = value;
    setNumberDraft(String(value));
  };
  const previewNumber = (raw: string) => {
    setNumberDraft(raw);
    const next = parseNumber(raw);
    if (next !== null && next >= setting.min && next <= setting.max) onPreview(normalize(next));
  };
  const finishNumber = (raw: string) => {
    const start = numberStart.current;
    if (start === null) return;
    numberStart.current = null;
    setNumberDraft(null);
    const parsed = parseNumber(raw);
    if (parsed === null) {
      onPreview(start);
      return;
    }
    const next = normalize(parsed);
    onPreview(next);
    if (next !== start) onCommit(next);
  };
  const cancelNumber = () => {
    const start = numberStart.current;
    numberStart.current = null;
    setNumberDraft(null);
    if (start !== null) onPreview(start);
  };
  const exactValueInput = (
    <input
      aria-label={settingLabel}
      id={setting.control === "number" ? setting.id : `${setting.id}-value`}
      inputMode="decimal"
      max={setting.max}
      min={setting.min}
      onBlur={(event) => finishNumber(event.currentTarget.value)}
      onChange={(event) => previewNumber(event.currentTarget.value)}
      onFocus={beginNumber}
      onKeyDown={(event) => {
        if (event.key === "Enter") {
          event.preventDefault();
          finishNumber(event.currentTarget.value);
          event.currentTarget.blur();
        } else if (event.key === "Escape") {
          event.preventDefault();
          cancelNumber();
          event.currentTarget.blur();
        }
      }}
      step={step}
      type="number"
      value={numberDraft ?? value}
    />
  );
  const controlClass = setting.control === "range"
    ? "range-control"
    : `range-control ${setting.control === "number" ? "number-control" : "discrete-control"}`;

  return (
    <div className={controlClass}>
      {setting.control === "select" && "options" in setting ? (
        <select id={setting.id} onChange={(event) => onCommit(Number(event.target.value))} value={value}>
          {setting.options.map((option) => (
            <option key={option.value} value={option.value}>
              {t(settingOptionMessageKey(setting.id, option.value))}
            </option>
          ))}
        </select>
      ) : setting.control === "boolean" ? (
        <label className="switch-control">
          <input checked={value === 1} id={setting.id} onChange={(event) => onCommit(event.target.checked ? 1 : 0)} type="checkbox" />
          <span>{value === 1 ? t("profiles.enabled") : t("profiles.disabled")}</span>
        </label>
      ) : setting.control === "number" ? exactValueInput : (
        <input
          id={setting.id}
          max={setting.max}
          min={setting.min}
          onBlur={(event) => finishRange(Number(event.currentTarget.value))}
          onChange={(event) => previewRange(Number(event.currentTarget.value))}
          onKeyDown={(event) => {
            if (rangeAdjustmentKeys.has(event.key)) beginRange();
          }}
          onKeyUp={(event) => {
            if (rangeAdjustmentKeys.has(event.key)) finishRange(Number(event.currentTarget.value));
          }}
          onPointerCancel={(event) => finishRange(Number(event.currentTarget.value))}
          onPointerDown={beginRange}
          onPointerUp={(event) => finishRange(Number(event.currentTarget.value))}
          step={step}
          type="range"
          value={value}
        />
      )}
      {setting.control === "range" && exactValueInput}
      <button className="icon-button" aria-label={t("profiles.reset", { setting: settingLabel })} onClick={() => onCommit(setting.default)} type="button">
        <RotateCcw aria-hidden="true" size={15} />
      </button>
    </div>
  );
}

function SchemaSettings({ settings, values, dirtyKeys, onChange, onPreviewChange, t }: SchemaSettingsProps) {
  return settings.map((setting) => {
    const value = values[setting.id] ?? setting.default;
    const dirty = dirtyKeys.includes(setting.id);
    const settingLabel = t(settingMessageKey(setting.id, "label"));
    const settingDescription = t(settingMessageKey(setting.id, "description"));
    return (
      <div className="setting-row" key={setting.id}>
        <div>
          <label htmlFor={setting.id}>
            {settingLabel} {dirty && <span className="dirty-mark">{t("profiles.changed")}</span>}
          </label>
          <p>
            {settingDescription} {t("profiles.settingMeta", { default: setting.default, min: setting.min, max: setting.max })}
            {setting.apply === "restart_required" ? ` · ${t("profiles.restartRequired")}` : ""}
          </p>
        </div>
        <SettingControl setting={setting} settingLabel={settingLabel} t={t} value={value} onCommit={(nextValue) => onChange(setting.id, nextValue)} onPreview={(nextValue) => onPreviewChange(setting.id, nextValue)} />
      </div>
    );
  });
}

export function BasicSettings(props: SchemaSettingsProps) {
  return <SchemaSettings {...props} />;
}

export function ShapeSettings(props: SchemaSettingsProps) {
  return <SchemaSettings {...props} />;
}

export function LcdSettings(props: SchemaSettingsProps) {
  return <SchemaSettings {...props} />;
}

export function SearchSettings(props: SchemaSettingsProps) {
  return <SchemaSettings {...props} />;
}

export { SchemaSettings };
