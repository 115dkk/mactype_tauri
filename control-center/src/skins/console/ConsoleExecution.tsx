import { ChevronDown, RefreshCw, ShieldAlert, Wrench } from "lucide-react";
import { useEffect, useState } from "react";
import { StatusDot } from "../../components/StatusDot";
import { SwitchControl } from "../../components/SwitchControl";
import { eventTime } from "../../features/events/eventText";
import { ExecutionMessages, LegacyServiceControls, LegacyTrayConflict, ManualLaunchBody, MigrationConfirmation, RegisteredTargetsBody, ServicePackageNotice, ServiceSummaryNoticeAndActions, SystemServiceControls } from "../../features/execution/ExecutionParts";
import { useI18n } from "../../i18n/i18n";
import { ConsoleFrame, ConsoleKv, ConsolePanel } from "./ConsoleFrame";
import { useConsole } from "./consoleContext";
import { ConsoleServiceStatus } from "./ConsoleStatus";
import { serviceTone } from "./serviceTone";

type Expanded = "registered" | "manual" | null;

export function ConsoleExecution() {
  const { locale, t } = useI18n();
  const { execution: model } = useConsole();
  const { status, serviceSummary, systemInjectionAction, legacyService } = model;
  const [expanded, setExpanded] = useState<Expanded>(null);
  const [checkedAt, setCheckedAt] = useState<number | null>(null);
  useEffect(() => {
    if (status) setCheckedAt(Date.now());
  }, [status]);
  const toggle = (kind: Exclude<Expanded, null>) => {
    const next = expanded === kind ? null : kind;
    setExpanded(next);
    if (next === "manual" && model.candidates === null) void model.loadCandidates();
  };
  const tone = serviceTone(serviceSummary.tone);

  return (
    <ConsoleFrame
      actions={<button className="button secondary" onClick={() => void model.refresh()} type="button"><RefreshCw aria-hidden="true" size={14} /> {t("execution.refresh")}</button>}
      bodyClassName="console-cols-side-main"
      crumb={t("nav.wizardGroup")}
      status={<ConsoleServiceStatus />}
      statusRight={checkedAt && <span className="app-statusbar-item">{t("execution.lastChecked", { time: eventTime(checkedAt, locale) })}</span>}
      summary={<>MacTypeControlCenter · {t(serviceSummary.modeKey)}</>}
      title={t("nav.execution")}
      titleId="execution-title"
    >
      <ConsolePanel
        className="console-service-panel"
        footer={<>
          <button className={`button ${systemInjectionAction.intent === "stop" ? "secondary" : "primary"}`} disabled={!systemInjectionAction.enabled} onClick={() => void model.manageService(systemInjectionAction.command)} type="button">{t(systemInjectionAction.labelKey)}</button>
          <span className="console-spacer" />
        </>}
        title={t("execution.statusPanel")}
      >
        <div className="console-big" data-service-summary data-state={serviceSummary.tone}>
          <StatusDot tone={tone} />
          {t(serviceSummary.statusKey)}
          <small>{model.serviceStateText}</small>
        </div>
        <ConsoleKv rows={[
          { key: "profile", label: t("execution.summaryProfile"), value: <code title={status?.activeProfile ?? undefined}>{model.activeProfileName}</code> },
          { key: "mode", label: t("execution.summaryMode"), value: t(serviceSummary.modeKey) },
          { key: "generation", label: t("execution.profileGeneration"), value: t(model.profileIndicator.labelKey) },
          { key: "appinit", label: t("execution.appInit"), value: status?.registryModeDetected ? t("execution.entryDetected") : t("execution.notDetected") },
          { key: "legacy", label: t("execution.legacyTrayLabel"), value: legacyService ? `${t(`execution.servicePresence.${legacyService.presence}`)} · ${t(`execution.serviceState.${legacyService.state}`)}` : t("execution.notDetected") },
        ]} />
        <LegacyTrayConflict model={model} />
        {!model.legacyTrayResolution && (serviceSummary.notice || serviceSummary.actions.length > 0) && <div className="console-summary-actions"><ServiceSummaryNoticeAndActions model={model} /></div>}
        <ServicePackageNotice model={model} />
        <details className="console-details">
          <summary><Wrench aria-hidden="true" size={13} /> {t("execution.maintenance")}<ChevronDown aria-hidden="true" className="console-details-chevron" size={13} /></summary>
          <SystemServiceControls model={model} />
          <LegacyServiceControls model={model} />
        </details>
        <div className="console-note"><ShieldAlert aria-hidden="true" size={14} />{status ? t("execution.systemNote") : t("execution.checking")}</div>
      </ConsolePanel>

      <ConsolePanel className="console-modes-panel" title={t("execution.modesPanel")}>
        <div className="console-mode-row" data-kind="system">
          <StatusDot tone={systemInjectionAction.state === "active" ? "ok" : "neutral"} />
          <div><h3 id="console-system-title">{t("execution.systemTitle")}</h3><p>{t("execution.systemDescription")}</p></div>
          <span className="console-mode-state">{t(systemInjectionAction.titleKey)}</span>
          <SwitchControl checked={systemInjectionAction.state === "active"} disabled={!systemInjectionAction.enabled} labelledBy="console-system-title" onChange={() => void model.manageService(systemInjectionAction.command)} />
        </div>
        <div className="console-mode-row" data-kind="autostart">
          <StatusDot tone={status?.autoStart ? "ok" : "neutral"} />
          <div><h3 id="console-autostart-title">{t("execution.autostartTitle")}</h3><p>{t("execution.autostartDescription")}</p></div>
          <span className="console-mode-state">{status?.autoStart ? t("common.on") : t("common.off")}</span>
          <SwitchControl checked={status?.autoStart ?? false} disabled={!status} labelledBy="console-autostart-title" onChange={(checked) => void model.toggleAutostart(checked)} />
        </div>
        <div className="console-mode-row" data-expanded={expanded === "registered"} data-kind="registered">
          <StatusDot tone={status?.sessionTargets.length ? "ok" : "neutral"} />
          <div><h3>{t("execution.registeredTitle")}</h3><p>{t("execution.registeredDescription")}</p></div>
          <span className="console-mode-state">{t("execution.registeredCount", { count: status?.sessionTargets.length ?? 0 })}</span>
          <button aria-expanded={expanded === "registered"} className="icon-button" onClick={() => toggle("registered")} type="button"><ChevronDown aria-hidden="true" size={14} /></button>
        </div>
        {expanded === "registered" && <div className="console-mode-detail"><RegisteredTargetsBody model={model} /></div>}
        <div className="console-mode-row" data-expanded={expanded === "manual"} data-kind="manual">
          <StatusDot tone={model.target ? "accent" : "neutral"} />
          <div><h3>{t("execution.manualTitle")}</h3><p>{t("execution.manualDescription")}</p></div>
          <span className="console-mode-state">{model.targetName || t("execution.noExecutableSelected")}</span>
          <button aria-expanded={expanded === "manual"} className="icon-button" onClick={() => toggle("manual")} type="button"><ChevronDown aria-hidden="true" size={14} /></button>
        </div>
        {expanded === "manual" && <div className="console-mode-detail"><ManualLaunchBody model={model} /></div>}
        <div className="console-messages"><ExecutionMessages model={model} /></div>
      </ConsolePanel>
      <MigrationConfirmation model={model} />
    </ConsoleFrame>
  );
}
