# Hint

A dotted-underline term that opens a small popover after 120ms of hover; the popover is `role="tooltip"` and closes on Escape.

- Props: `children` (the term), `content` (the description, with an optional `hint-meta` line) and `contentId` for `aria-describedby`.
- The popover is 360px at most, 8px from the viewport edge and 6px from its anchor, bordered by `color-border-strong` and lifted with `shadow-window`.
- Use it for a setting's description and range. Never hide something the reader must know to act; put that in visible help text.
