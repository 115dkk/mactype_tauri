import { RotateCcw } from "lucide-react";
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
  t: I18nValue["t"];
}

interface SettingControlProps {
  setting: SettingDefinition;
  settingLabel: string;
  value: number;
  onChange: (value: number) => void;
  t: I18nValue["t"];
}

function SettingControl({ setting, settingLabel, value, onChange, t }: SettingControlProps) {
  return (
    <div className="range-control">
      {setting.control === "select" && "options" in setting ? (
        <select id={setting.id} onChange={(event) => onChange(Number(event.target.value))} value={value}>
          {setting.options.map((option) => (
            <option key={option.value} value={option.value}>
              {t(settingOptionMessageKey(setting.id, option.value))}
            </option>
          ))}
        </select>
      ) : setting.control === "boolean" ? (
        <label className="switch-control">
          <input checked={value === 1} id={setting.id} onChange={(event) => onChange(event.target.checked ? 1 : 0)} type="checkbox" />
          <span>{value === 1 ? t("profiles.enabled") : t("profiles.disabled")}</span>
        </label>
      ) : (
        <input
          id={setting.id}
          max={setting.max}
          min={setting.min}
          onChange={(event) => onChange(Number(event.target.value))}
          step={setting.type === "integer" ? 1 : 0.01}
          type="range"
          value={value}
        />
      )}
      <output htmlFor={setting.id}>
        {value}{setting.unit === "px" ? " px" : ""}
      </output>
      <button className="icon-button" aria-label={t("profiles.reset", { setting: settingLabel })} onClick={() => onChange(setting.default)} type="button">
        <RotateCcw aria-hidden="true" size={15} />
      </button>
    </div>
  );
}

function SchemaSettings({ settings, values, dirtyKeys, onChange, t }: SchemaSettingsProps) {
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
        <SettingControl setting={setting} settingLabel={settingLabel} t={t} value={value} onChange={(nextValue) => onChange(setting.id, nextValue)} />
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
