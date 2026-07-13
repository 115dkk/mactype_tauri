import { AlertTriangle, ArrowRight, Check, FolderSearch, LoaderCircle } from "lucide-react";
import { useState } from "react";
import type { InstallationStatus } from "../app/model";
import { useI18n } from "../i18n/i18n";

interface OverviewPageProps {
  status: InstallationStatus;
  onOpenProfiles: () => void;
  onReconnect: () => Promise<InstallationStatus>;
  onRelocate: () => Promise<InstallationStatus>;
}

export function OverviewPage({ status, onOpenProfiles, onReconnect, onRelocate }: OverviewPageProps) {
  const { t } = useI18n();
  const [operation, setOperation] = useState<"relocate" | "reconnect" | null>(null);
  const [completed, setCompleted] = useState<"relocate" | "reconnect" | null>(null);
  const [error, setError] = useState<string | null>(null);
  const run = async (kind: "relocate" | "reconnect", action: () => Promise<InstallationStatus>) => {
    setOperation(kind);
    setCompleted(null);
    setError(null);
    try {
      await action();
      setCompleted(kind);
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught));
    } finally {
      setOperation(null);
    }
  };
  const findingLabel = (label: string, value: string) => {
    if (value === "MacType.dll") return t("finding.core32");
    if (value === "MacType64.dll") return t("finding.core64");
    if (value === "MacLoader.exe") return t("finding.loader");
    if (label === "preview") return t("finding.preview");
    return label;
  };
  return (
    <section className="page view-enter" aria-labelledby="overview-title">
      <header className="page-header">
        <div>
          <h1 id="overview-title">{t("nav.overview")}</h1>
          <p>{t("overview.subtitle")}</p>
        </div>
        <button aria-busy={operation === "relocate"} className="button secondary" disabled={operation !== null} onClick={() => void run("relocate", onRelocate)} type="button">
          {operation === "relocate" ? <LoaderCircle aria-hidden="true" className="spin" size={17} /> : <FolderSearch aria-hidden="true" size={17} />}
          {t("overview.relocate")}
        </button>
      </header>

      <div className="status-band" data-state={status.state}>
        {status.state === "ready" ? <Check aria-hidden="true" size={20} /> : <AlertTriangle aria-hidden="true" size={20} />}
        <div>
          <strong>{status.state === "ready" ? `${t("overview.checked")} · MacType ${status.coreVersion ?? ""}` : t("overview.previewNeeded")}</strong>
          <span>{status.state === "ready" ? status.root : t("overview.previewNeededDescription")}</span>
        </div>
        <button aria-busy={operation === "reconnect"} className="button primary" disabled={operation !== null} onClick={() => void run("reconnect", onReconnect)} type="button">
          {operation === "reconnect" && <LoaderCircle aria-hidden="true" className="spin" size={17} />}
          {t("overview.reconnect")}
        </button>
      </div>

      <div aria-live="polite">
        {completed && <p className="success-message" data-operation={completed}><Check aria-hidden="true" size={16} /> {completed === "relocate" ? t("overview.relocate") : t("overview.reconnect")}</p>}
        {error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {error}</p>}
      </div>

      <section className="section-block" aria-labelledby="installation-title">
        <div className="section-heading">
          <h2 id="installation-title">{t("overview.installation")}</h2>
          <code>{status.root ?? t("overview.noRoot")}</code>
        </div>
        <dl className="detail-list">
          {status.findings.map((finding) => (
            <div key={finding.label}>
              <dt>{findingLabel(finding.label, finding.value)}</dt>
              <dd>
                {finding.ok ? <Check className="success" aria-label={t("overview.checked")} size={17} /> : <AlertTriangle className="warning" aria-label={t("overview.attention")} size={17} />}
                <span>{finding.value === "waiting" ? t("finding.waiting") : finding.value === "connected" ? t("overview.checked") : finding.value}</span>
              </dd>
            </div>
          ))}
        </dl>
      </section>

      <section className="split-section" aria-labelledby="next-title">
        <div>
          <h2 id="next-title">{t("overview.next")}</h2>
          <p>{t("overview.nextDescription")}</p>
        </div>
        <button className="text-action" onClick={onOpenProfiles} type="button">
          {t("overview.openProfiles")} <ArrowRight aria-hidden="true" size={17} />
        </button>
      </section>
    </section>
  );
}
