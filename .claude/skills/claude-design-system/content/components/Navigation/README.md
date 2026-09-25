# Navigation

The classic navigation pane, pinned to the classic skin in this card: the product lockup, the six entries in their fixed groups, and the language, skin and theme controls at the bottom.

- Order is interface: Overview; Wizard (Profiles, Service); Tuner (Guided setup, All settings); Tools (Diagnostics). View ids (`files`, `execution`, `profiles`) never change even when labels do.
- `nav-item` is 40px with `space-2` padding and `radius-control` corners; hover and selection use `color-surface-subtle`, and the selected item adds a 3px `color-primary` inset marker and weight 600. Grouped items are `nav-subitem`s behind a hairline.
- Icons are lucide at 18px (17px for sub-items), stroke 1.8.
- The preferences are `PreferenceMenu` triggers (`Languages`, `Palette`) and the theme toggle; each menu is a listbox with arrow, Home/End, Enter, Escape and Tab handling that returns focus to its trigger.
- Fluent, Console and Cupertino replace this pane with their own navigation (see their cards and the Skin designs section) but keep the same entries, order and preference controls.
