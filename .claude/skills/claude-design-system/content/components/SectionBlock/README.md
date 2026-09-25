# SectionBlock

The classic page grammar: a `page-header` (h1, muted subtitle, page actions), then `section-block`s with a `section-heading` and a body such as a `detail-list`.

- `page-header` ends in a hairline; a `compact` variant tightens it for dense pages.
- `section-block` is a bordered `color-surface` block with `space-8` above it. It is not a card: no radius, no shadow.
- `detail-list` is a `dl` of two-column rows (a 160px-minimum muted term, the value) split by hairlines. A leading 17px `Check` in `color-success` or `AlertTriangle` in `color-warning` marks each finding, and the word beside it says the state.
- Paths and versions are `code` in the mono stack.
