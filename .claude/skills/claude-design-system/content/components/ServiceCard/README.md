# ServiceCard

The classic overview's answer to "is MacType running": a `section-block` with a state heading, four key facts and the recent-activity block beneath it.

- `overview-service-card` takes `data-state` `normal`, `inactive` or `problem`; the heading icon (`Check`, `Power`, `AlertTriangle` at 22px) turns `color-success`, `color-muted` or `color-warning`.
- A non-normal card adds a `button secondary` that opens the Service page. The card never shows fake metrics or charts.
- The four facts are the run profile (`code`), the mode, the status and the last applied time, in `overview-service-details` cells split by hairlines.
- The recent-activity block shows the newest event and expands in place to a timestamped list with `tabular-nums` times.
