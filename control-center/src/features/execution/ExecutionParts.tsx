import { AlertTriangle, Check, FileCode2, FolderOpen, LogOut, Play, PowerOff, RefreshCw, ShieldAlert, Trash2, UserPlus, Wrench } from "lucide-react";
import type { ExecutionModel } from "./useExecutionModel";

interface PartProps {
  model: ExecutionModel;
}

/* Bodies that every skin shows the same way: the migration dialog, the legacy
   tray conflict, the package notice, the service controls, the registered
   launcher list and the manual launcher. Skins style them; they do not
   re-implement them, so the behaviour proven by the gallery states holds
   under every skin. */

export function MigrationConfirmation({ model }: PartProps) {
  const { t } = model;
  if (!model.migrationConfirmationOpen) return null;
  return (
    <div className="confirmation-backdrop">
      <section
        aria-labelledby="migration-confirmation-title"
        aria-modal="true"
        className="migration-confirmation"
        onKeyDown={model.handleMigrationDialogKeyDown}
        role="dialog"
      >
        <div className="migration-confirmation-heading">
          <ShieldAlert aria-hidden="true" size={22} />
          <div>
            <h2 id="migration-confirmation-title">{t("execution.migrationConfirmTitle")}</h2>
            <p>{t("execution.migrationConfirmDescription")}</p>
          </div>
        </div>
        <ol>
          <li>{t("execution.migrationConfirmStrictCheck")}</li>
          <li>{t("execution.migrationConfirmBackup")}</li>
          <li>{t("execution.migrationConfirmSwitch")}</li>
          <li>{t("execution.migrationConfirmRollback")}</li>
        </ol>
        <div className="migration-confirmation-actions">
          <button className="button secondary" onClick={model.closeMigrationConfirmation} ref={model.migrationCancelRef} type="button">{t("execution.migrationCancel")}</button>
          <button className="button primary" disabled={!model.executionView.canMigrateLegacy} onClick={() => void model.confirmMigration()} type="button">{t("execution.migrationContinue")}</button>
        </div>
      </section>
    </div>
  );
}

export function LegacyTrayConflict({ model }: PartProps) {
  const { t, legacyTrayResolution } = model;
  if (!legacyTrayResolution) return null;
  return (
    <div className="legacy-tray-conflict" data-kind={legacyTrayResolution.kind} data-legacy-tray-conflict data-prominent-exception>
      <div className="legacy-tray-conflict-copy">
        <span className="legacy-tray-conflict-icon"><ShieldAlert aria-hidden="true" size={20} /></span>
        <div>
          <strong>{t(legacyTrayResolution.titleKey)}</strong>
          <p>{t(legacyTrayResolution.descriptionKey)}</p>
        </div>
      </div>
      <div className="legacy-tray-conflict-actions">
        <button className="button secondary" disabled={model.legacyTrayBusy !== null} onClick={() => void model.refresh()} type="button">
          <RefreshCw aria-hidden="true" size={16} /> {t("execution.legacyTrayCheckAgain")}
        </button>
        {legacyTrayResolution.canRequestExit && (
          <button className="button primary" disabled={model.legacyTrayBusy !== null} onClick={() => void model.exitLegacyTray()} type="button">
            <LogOut aria-hidden="true" size={16} /> {t("execution.legacyTrayExit")}
          </button>
        )}
        {legacyTrayResolution.canDisableStartup && (
          <button className="button primary" disabled={model.legacyTrayBusy !== null} onClick={() => void model.disableLegacyTrayStartup()} type="button">
            <PowerOff aria-hidden="true" size={16} /> {t("execution.legacyTrayDisableAutostart")}
          </button>
        )}
      </div>
    </div>
  );
}

export function ServiceSummaryNoticeAndActions({ model }: PartProps) {
  const { t, serviceSummary } = model;
  return (
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
            {model.serviceBusy === action.command ? t("execution.serviceWorking") : t(action.labelKey)}
          </button>
        ))}
      </div>
    </>
  );
}

export function ServicePackageNotice({ model }: PartProps) {
  const { t, servicePackageNotice, status } = model;
  if (!servicePackageNotice) return null;
  return (
    <div className="service-package-notice" role="status" data-service-package={status?.serviceManagementPackage} data-prominent-exception>
      <span className="service-package-notice-icon"><ShieldAlert aria-hidden="true" size={20} /></span>
      <div>
        <strong>{t(servicePackageNotice.titleKey)}</strong>
        <p>{t(servicePackageNotice.descriptionKey)}</p>
      </div>
    </div>
  );
}

export function SystemServiceDetails({ model }: PartProps) {
  const { t, status, systemInjectionAction, profileIndicator, serviceStateText } = model;
  return (
    <dl className="detail-list">
      <div><dt>{t("execution.openServiceStatus")}</dt><dd>{systemInjectionAction.state === "active" ? <Check className="success" size={17} /> : systemInjectionAction.state === "inactive" ? <PowerOff className="neutral-status" size={17} /> : <AlertTriangle className="warning" size={17} />}<span>{serviceStateText}</span></dd></div>
      <div><dt>{t("execution.profileGeneration")}</dt><dd>{profileIndicator.kind === "matched" ? <Check className="success" size={17} /> : profileIndicator.kind === "service-off" ? <PowerOff className="neutral-status" size={17} /> : <AlertTriangle className="warning" size={17} />}<span>{t(profileIndicator.labelKey)}</span></dd></div>
      <div><dt>{t("execution.appInit")}</dt><dd>{status?.registryModeDetected ? <ShieldAlert className="warning" size={17} /> : <Check className="success" size={17} />}<span>{status?.registryModeDetected ? t("execution.entryDetected") : t("profiles.disabled")}</span></dd></div>
    </dl>
  );
}

export function SystemServiceControls({ model }: PartProps) {
  const { t, executionView, service, serviceBusy } = model;
  return (
    <div className="service-controls">
      <div>
        <strong>{t("execution.openServiceControlTitle")}</strong>
        <p>{t("execution.openServiceControlDescription")}</p>
        {executionView.serviceBinaryPath && (
          <div className="service-path">
            <code title={executionView.serviceBinaryPath}>{executionView.serviceBinaryPath}</code>
            <button className="button secondary" onClick={() => void model.revealServiceLocation()} type="button">
              <FolderOpen aria-hidden="true" size={16} /> {t("execution.revealSystemService")}
            </button>
          </div>
        )}
        {service?.backend === "foreign" && <p className="warning-text">{t("execution.serviceForeign")}</p>}
      </div>
      <div className="service-actions">
        <button className="button secondary" disabled={!executionView.canInstall} onClick={() => void model.manageService("install")} type="button">{serviceBusy === "install" ? t("execution.serviceWorking") : t("execution.serviceInstall")}</button>
        <button className="button secondary" disabled={!executionView.canStart} onClick={() => void model.manageService("start")} type="button">{serviceBusy === "start" ? t("execution.serviceWorking") : t("execution.serviceStart")}</button>
        {executionView.serviceNeedsUpgrade && <button className="button secondary" disabled={!executionView.canUpgrade} onClick={() => void model.manageService("upgrade")} type="button">{serviceBusy === "upgrade" ? t("execution.serviceWorking") : t("execution.serviceUpgrade")}</button>}
        {executionView.serviceNeedsRepair && <button className="button secondary" disabled={!executionView.canRepair} onClick={() => void model.manageService("repair")} type="button">{serviceBusy === "repair" ? t("execution.serviceWorking") : t("execution.serviceRepair")}</button>}
        <button className="button secondary danger" disabled={!executionView.canRemove} onClick={() => void model.manageService("remove")} type="button">{serviceBusy === "remove" ? t("execution.serviceWorking") : t("execution.serviceRemove")}</button>
      </div>
    </div>
  );
}

export function LegacyServiceControls({ model }: PartProps) {
  const { t, legacyService, executionView, serviceBusy } = model;
  if (!legacyService) return null;
  return (
    <div className="service-controls legacy-service-controls" data-service-backend="legacy-mactray">
      <div>
        <strong>{t("execution.legacyServiceTitle")}</strong>
        <p>{t("execution.legacyServiceDescription")}</p>
        <span>{`${t(`execution.servicePresence.${legacyService.presence}`)} · ${t(`execution.serviceState.${legacyService.state}`)}`}</span>
        {legacyService.registryConflict && <p className="warning-text">{t("execution.serviceRegistryConflict")}</p>}
        {legacyService.presence === "foreign" && <p className="warning-text">{t("execution.legacyServiceForeignDescription")}</p>}
        {legacyService.presence === "inaccessible" && <p className="warning-text">{t("execution.legacyServiceUncertainDescription")}</p>}
      </div>
      <div className="service-actions">
        <button className="button secondary" disabled={!executionView.canMigrateLegacy} onClick={model.openMigrationConfirmation} type="button">{serviceBusy === "migrate-from-legacy" ? t("execution.serviceWorking") : t("execution.migrateLegacy")}</button>
        <button className="button secondary danger" disabled={!executionView.canRemoveLegacy} onClick={() => void model.manageService("remove-legacy")} type="button">{serviceBusy === "remove-legacy" ? t("execution.serviceWorking") : t("execution.removeLegacy")}</button>
      </div>
    </div>
  );
}

export function RegisteredTargetsBody({ model }: PartProps) {
  const { t, status } = model;
  return (
    <div className="registered-launchers">
      <div className="registered-heading"><button className="button secondary" disabled={!status?.injectionReady || !status.sessionTargets.length} onClick={() => void model.launchAll()} type="button"><Play aria-hidden="true" size={16} /> {t("execution.launchRegistered")}</button></div>
      {status?.sessionTargets.length ? <ul>{status.sessionTargets.map((entry) => <li key={entry.target}><code>{entry.target}</code><button aria-label={t("execution.removeTarget", { name: entry.target })} className="icon-button" onClick={() => void model.remove(entry.target)} type="button"><Trash2 aria-hidden="true" size={16} /></button></li>)}</ul> : <p className="empty-state">{t("execution.noRegistered")}</p>}
    </div>
  );
}

export function ManualLaunchBody({ model }: PartProps) {
  const { t, status, target, visibleCandidates } = model;
  return (
    <>
      <div className="process-picker">
        <div className="process-picker-heading">
          <strong>{t("execution.processListTitle")}</strong>
          <div className="process-picker-tools">
            <input aria-label={t("execution.processListFilter")} className="process-picker-filter" onChange={(event) => model.setCandidateFilter(event.target.value)} placeholder={t("execution.processListFilter")} type="search" value={model.candidateFilter} />
            <button className="button secondary" onClick={() => void model.loadCandidates()} type="button"><RefreshCw aria-hidden="true" size={16} /> {t("execution.processListRefresh")}</button>
          </div>
        </div>
        {visibleCandidates.length ? (
          <ul className="process-picker-list">
            {visibleCandidates.map((candidate) => (
              <li key={candidate.pid}>
                <label className="process-picker-row">
                  <input checked={target === candidate.path} name="manual-launch-candidate" onChange={() => model.setTarget(candidate.path)} type="radio" value={candidate.path} />
                  <strong>{candidate.name}</strong>
                  {candidate.windowTitle && <span className="process-picker-window">{candidate.windowTitle}</span>}
                  <span className="process-picker-pid">{t("execution.processPid", { pid: candidate.pid })}</span>
                </label>
              </li>
            ))}
          </ul>
        ) : (
          <p className="empty-state">{t("execution.processListEmpty")}</p>
        )}
      </div>
      <div className="manual-launcher">
        <div className="target-picker">
          <span>{t("execution.browseFallback")}</span>
          <div className="target-selection" data-empty={!target}>
            <FileCode2 aria-hidden="true" size={22} />
            <div>
              <strong>{model.targetName || t("execution.noExecutableSelected")}</strong>
              {target && <code title={target}>{target}</code>}
            </div>
            <button className="button secondary" onClick={() => void model.chooseTarget()} type="button"><FolderOpen aria-hidden="true" size={17} /> {target ? t("execution.changeExecutable") : t("execution.chooseExecutable")}</button>
          </div>
        </div>
        <label><span>{t("execution.arguments")}</span><textarea onChange={(event) => model.setArgumentsText(event.target.value)} placeholder={t("execution.argumentsPlaceholder")} rows={3} value={model.argumentsText} /></label>
        <div className="manual-actions"><button className="button secondary" disabled={!status?.injectionReady || !target.trim()} onClick={() => void model.register()} type="button"><UserPlus aria-hidden="true" size={17} /> {t("execution.register")}</button><button className="button primary" disabled={!status?.manualLauncherAvailable || !target.trim()} onClick={() => void model.launch()} type="button"><Play aria-hidden="true" size={17} /> {t("execution.launch")}</button></div>
      </div>
    </>
  );
}

export function ExecutionMessages({ model }: PartProps) {
  return (
    <>
      {model.message && <p className="success-message">{model.message}</p>}
      {model.error && <p className="inline-error"><AlertTriangle aria-hidden="true" size={15} /> {model.error}</p>}
    </>
  );
}
