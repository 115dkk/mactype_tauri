import { AlertTriangle, Check, ChevronDown, FileCode2, ListChecks, Power, PowerOff, RefreshCw, ServerCog, ShieldAlert, Wrench } from "lucide-react";
import { ExecutionMessages, LegacyServiceControls, LegacyTrayConflict, ManualLaunchBody, MigrationConfirmation, RegisteredTargetsBody, ServicePackageNotice, SystemServiceControls, SystemServiceDetails } from "../features/execution/ExecutionParts";
import { useExecutionModel } from "../features/execution/useExecutionModel";

export function ExecutionPage({ ciSmoke = false, onReady }: { ciSmoke?: boolean; onReady?: () => void }) {
  const model = useExecutionModel({ ciSmoke, onReady });
  const { t, status, serviceSummary, systemInjectionAction, serviceStateText, serviceBusy } = model;

  return (
    <section className="page view-enter" aria-labelledby="execution-title">
      <header className="page-header">
        <div><h1 id="execution-title">{t("nav.execution")}</h1><p>{t("execution.subtitle")}</p></div>
        <div className="header-actions">
          <button className="button secondary" onClick={() => void model.refresh()} type="button"><RefreshCw aria-hidden="true" size={16} /> {t("execution.refresh")}</button>
        </div>
      </header>

      <section className="service-summary" data-state={serviceSummary.tone} data-service-summary>
        <dl className="service-summary-grid">
          <div><dt>{t("execution.summaryProfile")}</dt><dd><code title={status?.activeProfile ?? undefined}>{model.activeProfileName}</code></dd></div>
          <div><dt>{t("execution.summaryMode")}</dt><dd>{t(serviceSummary.modeKey)}</dd></div>
          <div>
            <dt>{t("execution.summaryStatus")}</dt>
            <dd>{serviceSummary.tone === "normal" ? <Check className="success" aria-hidden="true" size={18} /> : serviceSummary.tone === "neutral" ? <PowerOff className="neutral-status" aria-hidden="true" size={18} /> : <AlertTriangle className="warning" aria-hidden="true" size={18} />}{t(serviceSummary.statusKey)}</dd>
          </div>
        </dl>
        {model.legacyTrayResolution ? <LegacyTrayConflict model={model} /> : (
          <>
            {serviceSummary.notice && (
              <div className="service-summary-notice" data-kind={serviceSummary.notice.kind} data-prominent-exception>
                {serviceSummary.notice.kind === "repair" ? <Wrench aria-hidden="true" size={19} /> : <ShieldAlert aria-hidden="true" size={19} />}
                <div className="service-summary-notice-copy">
                  <strong>{t(serviceSummary.notice.titleKey)}</strong>
                  {serviceSummary.notice.descriptionKey && <p>{t(serviceSummary.notice.descriptionKey)}</p>}
                </div>
              </div>
            )}
            <div className="service-summary-actions">
              {serviceSummary.actions.map((action) => (
                <button
                  className={`button ${action.tone === "primary" ? "primary" : "secondary"}${action.tone === "danger" ? " danger" : ""}`}
                  disabled={!action.enabled}
                  key={action.command}
                  onClick={() => model.runSummaryAction(action.command)}
                  ref={action.command === "migrate-from-legacy" ? model.migrationTriggerRef : undefined}
                  type="button"
                >
                  {serviceBusy === action.command ? t("execution.serviceWorking") : t(action.labelKey)}
                </button>
              ))}
            </div>
          </>
        )}
      </section>

      <div className="service-rows">

      <details className="service-row" data-kind="system">
        <summary>
          <span className="service-row-icon"><ServerCog aria-hidden="true" size={19} /></span>
          <span className="service-row-copy"><h2>{t("execution.systemTitle")}</h2><p>{t("execution.systemDescription")}</p></span>
          <span className="service-row-state" data-tone={serviceSummary.tone}>{serviceStateText}</span>
          <ChevronDown aria-hidden="true" className="service-row-chevron" size={17} />
        </summary>
        <div className="service-row-body">
        <div className="open-service-card" data-service-backend="open-source">
        <ServicePackageNotice model={model} />
        <div className="system-injection-control" data-active={systemInjectionAction.state === "active"} data-state={systemInjectionAction.state}>
          <div className="system-injection-state">
            <span className="system-injection-icon">{systemInjectionAction.intent === "stop" ? <Power aria-hidden="true" size={20} /> : <PowerOff aria-hidden="true" size={20} />}</span>
            <div>
              <span className="eyebrow">{t("execution.openServiceTitle")}</span>
              <strong>{t(systemInjectionAction.titleKey)}</strong>
              <p>{t(systemInjectionAction.descriptionKey)}</p>
            </div>
          </div>
          <button
            className={systemInjectionAction.intent === "stop" ? "button secondary system-injection-action" : "button primary system-injection-action"}
            disabled={!systemInjectionAction.enabled}
            onClick={() => void model.manageService(systemInjectionAction.command)}
            type="button"
          >
            {t(systemInjectionAction.labelKey)}
          </button>
        </div>
        <SystemServiceDetails model={model} />
        <SystemServiceControls model={model} />
        </div>
        <LegacyServiceControls model={model} />
        <div className="system-mode-note"><ShieldAlert aria-hidden="true" size={19} /><p>{status ? t("execution.systemNote") : t("execution.checking")}</p></div>
        </div>
      </details>

      <div className="service-row service-row-static" data-kind="autostart">
        <div className="service-row-head execution-option">
          <span className="service-row-icon"><Power aria-hidden="true" size={19} /></span>
          <span className="service-row-copy"><h2 id="autostart-title">{t("execution.autostartTitle")}</h2><p>{t("execution.autostartDescription")}</p></span>
          <label className="switch-control"><input aria-labelledby="autostart-title" checked={status?.autoStart ?? false} disabled={!status} onChange={(event) => void model.toggleAutostart(event.target.checked)} role="switch" type="checkbox" /><span aria-hidden="true">{status?.autoStart ? t("common.on") : t("common.off")}</span></label>
        </div>
      </div>

      <details className="service-row" data-kind="registered">
        <summary>
          <span className="service-row-icon"><ListChecks aria-hidden="true" size={19} /></span>
          <span className="service-row-copy"><h2>{t("execution.registeredTitle")}</h2><p>{t("execution.registeredDescription")}</p></span>
          <span className="service-row-state" data-tone={status?.sessionTargets.length ? "normal" : "neutral"}>{t("execution.registeredCount", { count: status?.sessionTargets.length ?? 0 })}</span>
          <ChevronDown aria-hidden="true" className="service-row-chevron" size={17} />
        </summary>
        <div className="service-row-body">
          <RegisteredTargetsBody model={model} />
        </div>
      </details>

      <details className="service-row" data-kind="manual" onToggle={(event) => { if (event.currentTarget.open && model.candidates === null) void model.loadCandidates(); }}>
        <summary>
          <span className="service-row-icon"><FileCode2 aria-hidden="true" size={19} /></span>
          <span className="service-row-copy"><h2>{t("execution.manualTitle")}</h2><p>{t("execution.manualDescription")}</p></span>
          <span className="service-row-state" data-tone="neutral">{model.targetName || t("execution.noExecutableSelected")}</span>
          <ChevronDown aria-hidden="true" className="service-row-chevron" size={17} />
        </summary>
        <div className="service-row-body">
          <ManualLaunchBody model={model} />
        </div>
      </details>

      </div>

      <ExecutionMessages model={model} />
      <MigrationConfirmation model={model} />
    </section>
  );
}
