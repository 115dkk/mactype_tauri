import { useEffect, useRef, useState, type ReactNode } from "react";

interface PreferenceMenuOption<T extends string> {
  value: T;
  label: string;
}

interface PreferenceMenuProps<T extends string> {
  icon: ReactNode;
  label: string;
  options: ReadonlyArray<PreferenceMenuOption<T>>;
  value: T;
  onChange: (next: T) => void;
  testId: string;
  optionAttribute: string;
}

/* One popup menu shape for every navigation preference (language, skin), so a
   skin restyles a single component and the two pickers can never drift apart. */
export function PreferenceMenu<T extends string>({ icon, label, options, value, onChange, testId, optionAttribute }: PreferenceMenuProps<T>) {
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const selected = options.find((option) => option.value === value) ?? options[0];

  useEffect(() => {
    if (!open) return undefined;
    const closeOutside = (event: PointerEvent) => {
      if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
    };
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      setOpen(false);
      triggerRef.current?.focus();
    };
    document.addEventListener("pointerdown", closeOutside);
    document.addEventListener("keydown", closeOnEscape);
    return () => {
      document.removeEventListener("pointerdown", closeOutside);
      document.removeEventListener("keydown", closeOnEscape);
    };
  }, [open]);

  const choose = (next: T) => {
    onChange(next);
    setOpen(false);
    triggerRef.current?.focus();
  };

  return (
    <div className="preference-control" ref={rootRef}>
      {icon}
      <div className="preference-picker">
        <button
          aria-expanded={open}
          aria-haspopup="listbox"
          aria-label={label}
          className="preference-trigger"
          data-testid={testId}
          onClick={() => setOpen((current) => !current)}
          ref={triggerRef}
          type="button"
        >
          {selected.label}
        </button>
        {open && (
          <div aria-label={label} className="preference-menu" role="listbox">
            {options.map((option) => (
              <button
                aria-selected={option.value === value}
                className="preference-option"
                key={option.value}
                onClick={() => choose(option.value)}
                role="option"
                type="button"
                {...{ [optionAttribute]: option.value }}
              >
                {option.label}
              </button>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}
