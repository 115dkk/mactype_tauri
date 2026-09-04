import { ChevronDown, ChevronRight } from "lucide-react";
import type { ReactNode } from "react";

interface FluentPageProps {
  title: string;
  titleId: string;
  crumb?: string;
  subtitle?: ReactNode;
  actions?: ReactNode;
  wide?: boolean;
  children: ReactNode;
}

/* A Settings page: a display-size title (with an optional dim crumb before
   it), a muted subtitle, page actions on the right, and a content column. */
export function FluentPage({ title, titleId, crumb, subtitle, actions, wide, children }: FluentPageProps) {
  return (
    <section aria-labelledby={titleId} className="fluent-page" data-wide={wide}>
      <div className="fluent-head-row">
        <div>
          <h1 className="fluent-title" id={titleId}>{crumb && <><span className="fluent-title-dim">{crumb}</span><ChevronRight aria-hidden="true" size={18} strokeWidth={2} /></>}<span>{title}</span></h1>
          {subtitle && <p className="fluent-sub">{subtitle}</p>}
        </div>
        {actions && <div className="fluent-head-actions">{actions}</div>}
      </div>
      <div className="fluent-col">{children}</div>
    </section>
  );
}

export function FluentSection({ title, hint, children }: { title: string; hint?: string; children: ReactNode }) {
  return (
    <>
      <h2 className="fluent-sect">{title}{hint && <span>{hint}</span>}</h2>
      {children}
    </>
  );
}

export function FluentCards({ children }: { children: ReactNode }) {
  return <div className="fluent-cards">{children}</div>;
}

interface FluentCardProps {
  icon?: ReactNode;
  title: ReactNode;
  description?: ReactNode;
  /* Right-side content: state text, a button, a switch. */
  action?: ReactNode;
  hero?: boolean;
  tone?: "normal" | "attention" | "critical" | "neutral";
  /* Expander cards carry sub-rows and a chevron. */
  expanded?: boolean;
  onToggle?: () => void;
  children?: ReactNode;
  /* Marks a card whose value differs from the saved one. */
  dirty?: boolean;
  testId?: string;
  dataKind?: string;
}

/* The WinUI SettingsCard: header icon, regular-weight title, muted
   description, control on the right. With `onToggle` it becomes a
   SettingsExpander whose sub-rows sit inside the same card. */
export function FluentCard({ icon, title, description, action, hero, tone, expanded, onToggle, children, dirty, testId, dataKind }: FluentCardProps) {
  return (
    <div className="fluent-card" data-dirty={dirty} data-expanded={expanded} data-hero={hero} data-kind={dataKind} data-testid={testId} data-tone={tone}>
      {icon && <span className="fluent-card-icon">{icon}</span>}
      <div className="fluent-card-copy">
        <div className="fluent-card-title">{title}</div>
        {description && <div className="fluent-card-desc">{description}</div>}
      </div>
      <div className="fluent-card-act">
        {action}
        {onToggle && <button aria-expanded={expanded} className="fluent-chev" onClick={onToggle} type="button"><ChevronDown aria-hidden="true" size={16} strokeWidth={1.6} /></button>}
      </div>
      {expanded && children && <div className="fluent-sub-rows">{children}</div>}
    </div>
  );
}

export function FluentSubRow({ title, description, action, children }: { title?: ReactNode; description?: ReactNode; action?: ReactNode; children?: ReactNode }) {
  return (
    <div className="fluent-sub-row">
      {(title || description) && <div>{title && <div className="fluent-card-title">{title}</div>}{description && <div className="fluent-card-desc">{description}</div>}</div>}
      {children}
      {action}
    </div>
  );
}

export function FluentState({ children, tone }: { children: ReactNode; tone?: "ok" | "warn" | "bad" }) {
  return <span className="fluent-state" data-tone={tone}>{children}</span>;
}
