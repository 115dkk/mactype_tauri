import { AlertTriangle, ArrowRight, Check, FolderSearch } from "lucide-react";
import type { InstallationStatus } from "../app/model";
import { useI18n } from "../i18n/i18n";

export function OverviewPage({ status, onOpenProfiles }: { status: InstallationStatus; onOpenProfiles: () => void }) {
  const { t } = useI18n();
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
        <button className="button secondary" type="button">
          <FolderSearch aria-hidden="true" size={17} />
          {t("overview.relocate")}
        </button>
      </header>

      <div className="status-band" data-state={status.state}>
        <AlertTriangle aria-hidden="true" size={20} />
        <div>
          <strong>{t("overview.previewNeeded")}</strong>
          <span>{t("overview.previewNeededDescription")}</span>
        </div>
        <button className="button primary" type="button">{t("overview.reconnect")}</button>
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
                <span>{finding.value === "waiting" ? t("finding.waiting") : finding.value}</span>
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
