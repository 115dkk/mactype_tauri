import { AlertTriangle, Check, PowerOff, ShieldAlert } from "lucide-react";
import { useState } from "react";
import type { ShellProps } from "../../app/shell";
import { SwitchControl } from "../../components/SwitchControl";
import { ExecutionMessages, LegacyServiceControls, LegacyTrayConflict, ManualLaunchBody, MigrationConfirmation, RegisteredTargetsBody, ServicePackageNotice, ServiceSummaryNoticeAndActions, SystemServiceControls, SystemServiceDetails } from "../../features/execution/ExecutionParts";
import { useExecutionModel } from "../../features/execution/useExecutionModel";
import { useI18n } from "../../i18n/i18n";
import { CupertinoFootnote, CupertinoGroup, CupertinoPage, CupertinoRow, CupertinoSection } from "./CupertinoParts";

type Detail = "system" | "registered" | "manual" | null;

export function CupertinoExecution({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
  const model = useExecutionModel({ ciSmoke: shell.ciSmoke, onReady: () => shell.reportReady("execution") });
  const { status, serviceSummary, systemInjectionAction } = model;
  const [detail, setDetail] = useState<Detail>(null);
  const open = (next: Exclude<Detail, null>) => {
    setDetail(next);
    if (next === "manual" && model.candidates === null) void model.loadCandidates();
  };
  const heroTone = serviceSummary.tone === "normal" ? "ok" : serviceSummary.tone === "neutral" ? "neutral" : "warn";
  const heroIcon = serviceSummary.tone === "normal" ? <Check aria-hidden="true" size={16} strokeWidth={3} /> : serviceSummary.tone === "neutral" ? <PowerOff aria-hidden="true" size={15} strokeWidth={2.4} /> : <AlertTriangle aria-hidden="true" size={15} strokeWidth={2.4} />;

  if (detail) {
    const title = detail === "system" ? t("execution.systemTitle") : detail === "registered" ? t("execution.registeredTitle") : t("execution.manualTitle");
    return (
      <CupertinoPage backLabel={t("nav.execution")} onBack={() => setDetail(null)} title={title} titleId="execution-title">
        {detail === "system" && (
          <>
            <CupertinoGroup dataKind="system-detail">
              <ServicePackageNotice model={model} />
              <CupertinoRow
                description={t(systemInjectionAction.descriptionKey)}
                hero
                leading={<span className="cupertino-okc" data-tone={systemInjectionAction.state === "active" ? "ok" : "neutral"}>{systemInjectionAction.state === "active" ? <Check aria-hidden="true" size={16} strokeWidth={3} /> : <PowerOff aria-hidden="true" size={15} strokeWidth={2.4} />}</span>}
                title={t(systemInjectionAction.titleKey)}
                value={<button className={`button ${systemInjectionAction.intent === "stop" ? "secondary" : "primary"}`} disabled={!systemInjectionAction.enabled} onClick={() => void model.manageService(systemInjectionAction.command)} type="button">{t(systemInjectionAction.labelKey)}</button>}
              />
              <div className="cupertino-row cupertino-row-block"><SystemServiceDetails model={model} /></div>
            </CupertinoGroup>
            <CupertinoSection title={t("execution.maintenance")}>
              <CupertinoGroup dataKind="maintenance">
                <div className="cupertino-row cupertino-row-block"><SystemServiceControls model={model} /></div>
                <LegacyServiceControls model={model} />
              </CupertinoGroup>
            </CupertinoSection>
            <CupertinoFootnote>{status ? t("execution.systemNote") : t("execution.checking")}</CupertinoFootnote>
          </>
        )}
        {detail === "registered" && <CupertinoGroup dataKind="registered-detail"><div className="cupertino-row cupertino-row-block"><RegisteredTargetsBody model={model} /></div></CupertinoGroup>}
        {detail === "manual" && <CupertinoGroup dataKind="manual-detail"><div className="cupertino-row cupertino-row-block cupertino-manual"><ManualLaunchBody model={model} /></div></CupertinoGroup>}
        <ExecutionMessages model={model} />
        <MigrationConfirmation model={model} />
      </CupertinoPage>
    );
  }

  return (
    <CupertinoPage
      actions={<button className="button secondary" onClick={() => void model.refresh()} type="button">{t("execution.refresh")}</button>}
      subtitle={t("execution.subtitle")}
      title={t("nav.execution")}
      titleId="execution-title"
    >
      <div data-service-summary data-state={serviceSummary.tone}>
        <CupertinoGroup dataKind="hero">
          <CupertinoRow
            dataKind="hero"
            description={<>{t("execution.summaryProfile")} <code title={status?.activeProfile ?? undefined}>{model.activeProfileName}</code> · {model.serviceStateText}</>}
            hero
            leading={<span className="cupertino-okc" data-tone={heroTone}>{heroIcon}</span>}
            title={t(serviceSummary.statusKey)}
            value={!model.legacyTrayResolution && <div className="cupertino-summary-actions"><ServiceSummaryNoticeAndActions model={model} /></div>}
          />
          {serviceSummary.notice && !model.legacyTrayResolution && <div className="cupertino-row cupertino-row-notice"><ShieldAlert aria-hidden="true" size={14} strokeWidth={1.8} /><div><strong>{t(serviceSummary.notice.titleKey)}</strong>{serviceSummary.notice.descriptionKey && <p>{t(serviceSummary.notice.descriptionKey)}</p>}</div></div>}
          <LegacyTrayConflict model={model} />
        </CupertinoGroup>
      </div>

      <CupertinoSection title={t("execution.modesPanel")}>
        <CupertinoGroup dataKind="modes">
          <CupertinoRow dataKind="system" description={t("execution.systemDescription")} onDisclose={() => open("system")} title={t("execution.systemTitle")} value={<span className="cupertino-value" data-tone={systemInjectionAction.state === "active" ? "ok" : undefined}>{t(systemInjectionAction.titleKey)}</span>} />
          <CupertinoRow dataKind="autostart" description={t("execution.autostartDescription")} title={<span id="cupertino-autostart-title">{t("execution.autostartTitle")}</span>} value={<SwitchControl checked={status?.autoStart ?? false} disabled={!status} labelledBy="cupertino-autostart-title" onChange={(checked) => void model.toggleAutostart(checked)} />} />
          <CupertinoRow dataKind="registered" description={t("execution.registeredDescription")} onDisclose={() => open("registered")} title={t("execution.registeredTitle")} value={t("execution.registeredCount", { count: status?.sessionTargets.length ?? 0 })} />
          <CupertinoRow dataKind="manual" description={t("execution.manualDescription")} onDisclose={() => open("manual")} title={t("execution.manualTitle")} value={model.targetName || t("execution.noExecutableSelected")} />
        </CupertinoGroup>
        <CupertinoFootnote>{status ? t("execution.systemNote") : t("execution.checking")}</CupertinoFootnote>
      </CupertinoSection>
      <ExecutionMessages model={model} />
      <MigrationConfirmation model={model} />
    </CupertinoPage>
  );
}
