# Navigation

The navigation pane: the product lockup, the six entries in their fixed groups, and the language picker and theme toggle at the bottom.

- Order is interface: Overview; Wizard (Profiles, Service); Tuner (Guided setup, All settings); Diagnostics. View ids (`files`, `execution`, `profiles`) never change even when labels do.
- `nav-item` is 40px with `space-2` padding and `radius-control` corners; hover and selection use `color-surface-subtle`, and the selected item adds a 3px `color-primary` inset marker and weight 600. Grouped items are `nav-subitem`s behind a hairline.
- Icons are lucide at 18px (17px for sub-items), stroke 1.8.
- The language picker (`language-control`, a 17px `Languages` icon and a trigger) opens a listbox of the ten locales, closes on Escape or an outside click and returns focus to its trigger. The theme toggle names the theme it switches to.
