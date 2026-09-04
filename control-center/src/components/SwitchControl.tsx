interface SwitchControlProps {
  checked: boolean;
  disabled?: boolean;
  onChange: (checked: boolean) => void;
  /* Accessible name; when `labelledBy` is given it wins. */
  label?: string;
  labelledBy?: string;
  /* Visible state word beside the switch (On/Off); omitted in dense skins. */
  stateText?: string;
}

/* The one switch markup every skin shares: a checkbox with the switch role,
   so the gallery finds it the same way everywhere. */
export function SwitchControl({ checked, disabled, onChange, label, labelledBy, stateText }: SwitchControlProps) {
  return (
    <label className="switch-control">
      <input aria-label={labelledBy ? undefined : label} aria-labelledby={labelledBy} checked={checked} disabled={disabled} onChange={(event) => onChange(event.target.checked)} role="switch" type="checkbox" />
      {stateText !== undefined && <span aria-hidden="true">{stateText}</span>}
    </label>
  );
}
