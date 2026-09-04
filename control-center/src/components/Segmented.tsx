interface SegmentedOption<T extends string> {
  value: T;
  label: string;
}

interface SegmentedProps<T extends string> {
  label: string;
  options: ReadonlyArray<SegmentedOption<T>>;
  value: T;
  onChange: (value: T) => void;
  compact?: boolean;
}

/* A two-to-four way choice drawn as one control. The selected segment is
   the only filled one; skins restyle it through `.segmented`. */
export function Segmented<T extends string>({ label, options, value, onChange, compact }: SegmentedProps<T>) {
  return (
    <div aria-label={label} className="segmented" data-compact={compact} role="radiogroup">
      {options.map((option) => (
        <button aria-checked={option.value === value} className="segmented-option" key={option.value} onClick={() => onChange(option.value)} role="radio" type="button">{option.label}</button>
      ))}
    </div>
  );
}
