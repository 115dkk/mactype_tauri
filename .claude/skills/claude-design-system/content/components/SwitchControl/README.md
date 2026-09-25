# SwitchControl

The one switch every skin shares: a native checkbox with `role="switch"` inside `label.switch-control`, followed by an optional state word.

- Markup: `<label class="switch-control"><input type="checkbox" role="switch" aria-label="…"><span aria-hidden="true">On</span></label>`. The React component takes `checked`, `onChange`, `disabled`, `label` or `labelledBy`, and `stateText`.
- Geometry comes only from `switch-width`, `switch-height`, `switch-thumb`, `switch-inset` and `switch-border`; the thumb positions are derived in LTR and RTL. A skin changes those metrics and never adds a transform.
- Checked fills the track with `color-primary` and the thumb with `color-on-primary`. Disabled uses `opacity-disabled` on the track and the state word.
- Console and Cupertino hide the state word; Fluent places it before the switch. Use a switch for an immediate on/off setting, never for a choice that needs Save.
