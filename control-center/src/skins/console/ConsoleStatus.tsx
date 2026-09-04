import { StatusDot } from "../../components/StatusDot";
import { serviceTone } from "./serviceTone";
import { useI18n } from "../../i18n/i18n";
import { useConsole } from "./consoleContext";

/* The left half of every Console status bar: service state, profile, and
   the preview helper connection, so a page change never loses the context. */
export function ConsoleServiceStatus() {
  const { t } = useI18n();
  const { execution, shell } = useConsole();
  const preview = shell.status.findings.find((finding) => finding.label === "preview");
  const helperConnected = preview?.value === "connected";
  return (
    <>
      <span className="app-statusbar-item"><StatusDot tone={serviceTone(execution.serviceSummary.tone)} /> {t(execution.serviceSummary.modeKey)} · {t(execution.serviceSummary.statusKey)}</span>
      <span className="app-statusbar-item"><code>{execution.activeProfileName}</code></span>
      <span className="app-statusbar-item" data-ok={helperConnected}><StatusDot tone={helperConnected ? "ok" : "warn"} /> {t("finding.preview")} · {helperConnected ? t("overview.checked") : t("finding.waiting")}</span>
    </>
  );
}
