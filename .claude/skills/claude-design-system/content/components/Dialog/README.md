# Dialog

A modal `profile-name-dialog` over a `confirmation-backdrop`: a title, one sentence of consequence, the field, a live validation line and the actions.

- The dialog is `role="dialog"` with `aria-modal`, traps Tab, closes on Escape and returns focus to its trigger. The field is focused and selected on open.
- The validation message is `aria-live="polite"` and names the problem ("A profile with that name already exists."); the submit button stays disabled while the name is invalid.
- Actions sit at the trailing edge: `button secondary` Cancel, then the one `button primary`.
- Only floating layers take `shadow-window`; the dialog is one of them.
