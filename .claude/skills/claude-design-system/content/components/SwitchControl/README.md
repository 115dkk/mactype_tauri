# SwitchControl

The switch the pages write inline: a native checkbox with `role="switch"` inside `label.switch-control`, followed by the state word.

- Markup: `<label class="switch-control"><input type="checkbox" role="switch" aria-labelledby="…"><span aria-hidden="true">On</span></label>`. The state word is always visible ("On", "Off").
- The track is 40×20 with a 1px `color-border-strong` edge and a 12px thumb inset 3px; hover grows the thumb to 14px. Checked fills the track with `color-primary` and the thumb with `color-on-primary`, and the thumb positions are logical, so RTL mirrors them.
- Disabled uses `opacity-disabled` on the track and the state word and keeps the thumb at its resting size.
- Use a switch for an immediate on/off setting, never for a choice that needs Save.
