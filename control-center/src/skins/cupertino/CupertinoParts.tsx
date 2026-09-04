import { ChevronLeft, ChevronRight } from "lucide-react";
import type { ReactNode } from "react";

interface CupertinoPageProps {
  title: string;
  titleId: string;
  subtitle?: ReactNode;
  actions?: ReactNode;
  wide?: boolean;
  /* A detail page shows a back control before the title instead of the subtitle. */
  onBack?: () => void;
  backLabel?: string;
  children: ReactNode;
}

/* A System Settings page: a bold 24-pixel title, a muted subtitle, actions at
   the trailing edge, and a 700-pixel content column. */
export function CupertinoPage({ title, titleId, subtitle, actions, wide, onBack, backLabel, children }: CupertinoPageProps) {
  return (
    <section aria-labelledby={titleId} className="cupertino-page" data-wide={wide}>
      <div className="cupertino-hd">
        <div>
          {onBack && <button className="cupertino-back" onClick={onBack} type="button"><ChevronLeft aria-hidden="true" size={16} strokeWidth={2} />{backLabel}</button>}
          <h1 id={titleId}>{title}</h1>
          {subtitle && <p className="cupertino-sub">{subtitle}</p>}
        </div>
        {actions && <div className="cupertino-hd-actions">{actions}</div>}
      </div>
      <div className="cupertino-col">{children}</div>
    </section>
  );
}

export function CupertinoSection({ title, children }: { title: string; children: ReactNode }) {
  return (
    <>
      <h2 className="cupertino-sect">{title}</h2>
      {children}
    </>
  );
}

export function CupertinoGroup({ children, className, dataKind }: { children: ReactNode; className?: string; dataKind?: string }) {
  return <div className={className ? `cupertino-group ${className}` : "cupertino-group"} data-kind={dataKind}>{children}</div>;
}

interface CupertinoRowProps {
  /* Leading content: a radio, an icon tile, a status circle. */
  leading?: ReactNode;
  title?: ReactNode;
  description?: ReactNode;
  /* Trailing content: a value, a badge, a switch, a button. */
  value?: ReactNode;
  /* A disclosure row navigates somewhere on click. */
  onDisclose?: () => void;
  hero?: boolean;
  dirty?: boolean;
  dataKind?: string;
  children?: ReactNode;
}

/* One row of a group: leading slot, title and description, trailing value,
   and the chevron of a disclosure row. Hairline separators are drawn by the
   group, inset past the leading slot. */
export function CupertinoRow({ leading, title, description, value, onDisclose, hero, dirty, dataKind, children }: CupertinoRowProps) {
  const body = (
    <>
      {leading}
      <div className="cupertino-row-copy">
        {title && <div className="cupertino-row-title">{title}</div>}
        {description && <div className="cupertino-row-desc">{description}</div>}
        {children}
      </div>
      {(value || onDisclose) && <div className="cupertino-row-value">{value}{onDisclose && <ChevronRight aria-hidden="true" className="cupertino-chev" size={14} strokeWidth={2} />}</div>}
    </>
  );
  if (onDisclose) {
    return <button className="cupertino-row cupertino-row-disclose" data-dirty={dirty} data-hero={hero} data-kind={dataKind} data-leading={Boolean(leading)} onClick={onDisclose} type="button">{body}</button>;
  }
  return <div className="cupertino-row" data-dirty={dirty} data-hero={hero} data-kind={dataKind} data-leading={Boolean(leading)}>{body}</div>;
}

export function CupertinoStatusCircle({ tone }: { tone: "ok" | "warn" | "neutral" }) {
  return <span className="cupertino-okc" data-tone={tone} aria-hidden="true" />;
}

export function CupertinoBadge({ children }: { children: ReactNode }) {
  return <span className="cupertino-badge">{children}</span>;
}

export function CupertinoFootnote({ children }: { children: ReactNode }) {
  return <p className="cupertino-footnote">{children}</p>;
}

export function CupertinoToolbar({ children }: { children: ReactNode }) {
  return <div className="cupertino-toolbar">{children}</div>;
}
