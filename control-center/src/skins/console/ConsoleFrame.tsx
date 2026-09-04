import { ChevronRight } from "lucide-react";
import type { CSSProperties, ReactNode } from "react";

interface ConsoleFrameProps {
  crumb?: string;
  title: string;
  titleId: string;
  summary?: ReactNode;
  actions?: ReactNode;
  status?: ReactNode;
  statusRight?: ReactNode;
  bodyStyle?: CSSProperties;
  bodyClassName?: string;
  children: ReactNode;
}

/* The Console page frame: a command bar with the breadcrumb and the page's
   actions, a panel body, and a status bar the page fills with its own
   context. Every Console page contributes all three. */
export function ConsoleFrame({ crumb, title, titleId, summary, actions, status, statusRight, bodyStyle, bodyClassName, children }: ConsoleFrameProps) {
  return (
    <section aria-labelledby={titleId} className="console-page">
      <div className="console-bar">
        <div className="console-crumb">
          {crumb && <><span className="console-crumb-dim">{crumb}</span><ChevronRight aria-hidden="true" size={12} strokeWidth={2} /></>}
          <h1 id={titleId}>{title}</h1>
        </div>
        {summary && <div className="console-sub">{summary}</div>}
        <div className="console-bar-right">{actions}</div>
      </div>
      <div className={bodyClassName ? `console-body ${bodyClassName}` : "console-body"} style={bodyStyle}>{children}</div>
      <footer className="app-statusbar console-status" data-testid="app-statusbar">
        {status}
        <span className="app-statusbar-spacer" />
        {statusRight}
      </footer>
    </section>
  );
}

interface ConsolePanelProps {
  title: ReactNode;
  icon?: ReactNode;
  right?: ReactNode;
  footer?: ReactNode;
  className?: string;
  children: ReactNode;
  /* Panels whose body scrolls independently. */
  scroll?: boolean;
}

export function ConsolePanel({ title, icon, right, footer, className, children, scroll = true }: ConsolePanelProps) {
  return (
    <section className={className ? `console-panel ${className}` : "console-panel"}>
      <header className="console-panel-head">{icon}<span>{title}</span>{right && <div className="console-panel-head-right">{right}</div>}</header>
      <div className="console-panel-body" data-scroll={scroll}>{children}</div>
      {footer && <div className="console-panel-foot">{footer}</div>}
    </section>
  );
}

export interface KeyValueRow {
  key: string;
  label: ReactNode;
  value: ReactNode;
}

export function ConsoleKv({ rows }: { rows: ReadonlyArray<KeyValueRow> }) {
  return (
    <dl className="console-kv">
      {rows.map((row) => <div key={row.key}><dt>{row.label}</dt><dd>{row.value}</dd></div>)}
    </dl>
  );
}
