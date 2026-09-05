import { useEffect, useId, useRef, useState, type KeyboardEvent, type ReactNode } from "react";

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

/* Language and skin share a single semantic trigger and popup. */
export function PreferenceMenu<T extends string>({ icon, label, options, value, onChange, testId, optionAttribute }: PreferenceMenuProps<T>) {
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const menuRef = useRef<HTMLDivElement>(null);
  const menuId = useId();
  const selected = options.find((option) => option.value === value) ?? options[0];

  useEffect(() => {
    if (!open) return undefined;
    menuRef.current?.querySelector<HTMLElement>('[aria-selected="true"]')?.focus();
    const closeOutside = (event: PointerEvent) => {
      if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
    };
    const closeOnEscape = (event: globalThis.KeyboardEvent) => {
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
    const trigger = triggerRef.current;
    onChange(next);
    setOpen(false);
    trigger?.focus();
    // Switching skins replaces the shell, including this trigger.
    requestAnimationFrame(() => {
      if (!trigger?.isConnected) document.querySelector<HTMLButtonElement>(`[data-testid="${testId}"]`)?.focus();
    });
  };

  const navigateOptions = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key === "Tab") {
      setOpen(false);
      triggerRef.current?.focus();
      return;
    }
    const buttons = Array.from(event.currentTarget.querySelectorAll<HTMLButtonElement>('[role="option"]'));
    const index = buttons.indexOf(document.activeElement as HTMLButtonElement);
    const next = event.key === "Home" ? 0 : event.key === "End" ? buttons.length - 1
      : event.key === "ArrowDown" ? (index + 1) % buttons.length
        : event.key === "ArrowUp" ? (index - 1 + buttons.length) % buttons.length : -1;
    if (next >= 0) {
      event.preventDefault();
      buttons[next]?.focus();
    }
  };

  return (
    <div className="preference-control" ref={rootRef} onBlur={(event) => {
      if (!event.currentTarget.contains(event.relatedTarget)) setOpen(false);
    }}>
      <div className="preference-picker">
        <button
          aria-controls={open ? menuId : undefined}
          aria-expanded={open}
          aria-haspopup="listbox"
          aria-label={label}
          className="preference-trigger"
          data-testid={testId}
          onClick={() => setOpen((current) => !current)}
          onKeyDown={(event) => {
            if (event.key === "ArrowDown" || event.key === "ArrowUp") {
              event.preventDefault();
              setOpen(true);
            }
          }}
          ref={triggerRef}
          title={`${label}: ${selected.label}`}
          type="button"
        >
          {icon}
          <span>{selected.label}</span>
        </button>
        {open && (
          <div aria-label={label} className="preference-menu" id={menuId} onKeyDown={navigateOptions} ref={menuRef} role="listbox">
            {options.map((option) => (
              <button
                aria-selected={option.value === value}
                className="preference-option"
                key={option.value}
                onClick={() => choose(option.value)}
                role="option"
                tabIndex={option.value === value ? 0 : -1}
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
