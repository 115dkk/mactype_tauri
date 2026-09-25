# FluentCard

The Fluent skin's unit, pinned to Fluent in this card: the WinUI settings card with a 24px icon column, a regular-weight title, a muted description and the control at the trailing edge.

- Props: `icon`, `title`, `description`, `action` (state text, button or switch), `hero` (28px icon, 20px title), `tone` (`normal`, `attention`, `critical`, `neutral`), `dirty`, and `expanded` with `onToggle` to become an expander whose `FluentSubRow`s sit on the subtle surface indented 56px.
- Cards are at least 62px tall, stack in `fluent-cards` with small gaps and are introduced by a `fluent-sect` heading (14/600).
- State text is `FluentState` in the muted colour, or `color-success`, `color-warning`, `color-destructive` by tone. Titles are never bold; hierarchy comes from size and colour.
- Controls keep Fluent's 32px height and darker bottom edge (`fluent-control-bottom`) instead of a shadow.
