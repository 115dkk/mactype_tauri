import { AlertTriangle, CircleCheck, Info, Layers, ListChecks, Power, PowerOff, RefreshCw, Terminal } from "lucide-react";
import { useState } from "react";
import type { ShellProps } from "../../app/shell";
import { SwitchControl } from "../../components/SwitchControl";
import { ExecutionMessages, LegacyServiceControls, LegacyTrayConflict, ManualLaunchBody, MigrationConfirmation, RegisteredTargetsBody, ServicePackageNotice, ServiceSummaryNoticeAndActions, SystemServiceControls } from "../../features/execution/ExecutionParts";
import { useExecutionModel } from "../../features/execution/useExecutionModel";
import { useI18n } from "../../i18n/i18n";
import { FluentCard, FluentCards, FluentPage, FluentSection, FluentState, FluentSubRow } from "./FluentParts";

type Expanded = "system" | "registered" | "manual" | null;

export function FluentExecution({ shell }: { shell: ShellProps }) {
  const { t } = useI18n();
  const model = useExecutionModel({ ciSmoke: shell.ciSmoke, onReady: () => shell.reportReady("execution") });
  const { status, serviceSummary, systemInjectionAction } = model;
  const [expanded, setExpanded] = useState<Expanded>("system");
  const toggle = (kind: Exclude<Expanded, null>) => {
    const next = expanded === kind ? null : kind;
    setExpanded(next);
    if (next === "manual" && model.candidates === null) void model.loadCandidates();
  };
  const heroIcon = serviceSummary.tone === "normal"
    ? <CircleCheck aria-hidden="true" size={28} strokeWidth={1.6} />
    : serviceSummary.tone === "neutral"
      ? <PowerOff aria-hidden="true" size={28} strokeWidth={1.6} />
      : <AlertTriangle aria-hidden="true" size={28} strokeWidth={1.6} />;

  return (
    <FluentPage
      actions={<button className="button secondary" onClick={() => void model.refresh()} type="button"><RefreshCw aria-hidden="true" size={16} strokeWidth={1.6} /> {t("execution.refresh")}</button>}
      subtitle={t("execution.subtitle")}
      title={t("nav.execution")}
      titleId="execution-title"
    >
      <FluentCards>
        <div data-service-summary data-state={serviceSummary.tone}>
          <FluentCard
            action={!model.legacyTrayResolution && <div className="fluent-summary-actions"><ServiceSummaryNoticeAndActions model={model} /></div>}
            dataKind="hero"
            description={<>{t("execution.summaryProfile")} <code title={status?.activeProfile ?? undefined}>{model.activeProfileName}</code> · {model.serviceStateText}</>}
            hero
            icon={heroIcon}
            title={t(serviceSummary.statusKey)}
            tone={serviceSummary.tone}
          />
          <LegacyTrayConflict model={model} />
        </div>
      </FluentCards>

      <FluentSection title={t("execution.modesPanel")}>
        <FluentCards>
          <FluentCard
            action={<FluentState tone={systemInjectionAction.state === "active" ? "ok" : undefined}>{t(systemInjectionAction.titleKey)}</FluentState>}
            dataKind="system"
            description={t("execution.systemDescription")}
            expanded={expanded === "system"}
            icon={<Layers aria-hidden="true" size={20} strokeWidth={1.6} />}
            onToggle={() => toggle("system")}
            title={t("execution.systemTitle")}
          >
            <ServicePackageNotice model={model} />
            <FluentSubRow
              action={<button className={`button ${systemInjectionAction.intent === "stop" ? "secondary" : "primary"}`} disabled={!systemInjectionAction.enabled} onClick={() => void model.manageService(systemInjectionAction.command)} type="button">{t(systemInjectionAction.labelKey)}</button>}
              description={t(systemInjectionAction.descriptionKey)}
              title={t(systemInjectionAction.titleKey)}
            />
            <FluentSubRow action={<FluentState>{t(model.profileIndicator.labelKey)}</FluentState>} title={t("execution.profileGeneration")} />
            <FluentSubRow action={<FluentState tone={status?.registryModeDetected ? "warn" : undefined}>{status?.registryModeDetected ? t("execution.entryDetected") : t("execution.notDetected")}</FluentState>} description={t("execution.appInitDetectOnly")} title={t("execution.appInit")} />
            <SystemServiceControls model={model} />
            <LegacyServiceControls model={model} />
            <FluentSubRow><div className="fluent-card-desc fluent-info"><Info aria-hidden="true" size={14} strokeWidth={1.6} /> {status ? t("execution.systemNote") : t("execution.checking")}</div></FluentSubRow>
          </FluentCard>
          <FluentCard
            action={<div className="fluent-switch"><span>{status?.autoStart ? t("common.on") : t("common.off")}</span><SwitchControl checked={status?.autoStart ?? false} disabled={!status} labelledBy="fluent-autostart-title" onChange={(checked) => void model.toggleAutostart(checked)} /></div>}
            dataKind="autostart"
            description={t("execution.autostartDescription")}
            icon={<Power aria-hidden="true" size={20} strokeWidth={1.6} />}
            title={<span id="fluent-autostart-title">{t("execution.autostartTitle")}</span>}
          />
          <FluentCard
            action={<FluentState>{t("execution.registeredCount", { count: status?.sessionTargets.length ?? 0 })}</FluentState>}
            dataKind="registered"
            description={t("execution.registeredDescription")}
            expanded={expanded === "registered"}
            icon={<ListChecks aria-hidden="true" size={20} strokeWidth={1.6} />}
            onToggle={() => toggle("registered")}
            title={t("execution.registeredTitle")}
          >
            <RegisteredTargetsBody model={model} />
          </FluentCard>
          <FluentCard
            action={<FluentState>{model.targetName || t("execution.noExecutableSelected")}</FluentState>}
            dataKind="manual"
            description={t("execution.manualDescription")}
            expanded={expanded === "manual"}
            icon={<Terminal aria-hidden="true" size={20} strokeWidth={1.6} />}
            onToggle={() => toggle("manual")}
            title={t("execution.manualTitle")}
          >
            <ManualLaunchBody model={model} />
          </FluentCard>
        </FluentCards>
      </FluentSection>
      <ExecutionMessages model={model} />
      <MigrationConfirmation model={model} />
    </FluentPage>
  );
}
