import { ProfileNameDialog } from "../../components/ProfileNameDialog";
import { useI18n } from "../../i18n/i18n";
import { AlertTriangle, BadgeCheck, Check, Play } from "lucide-react";
import type { ProfileEntry } from "../../app/model";
import type { FileSettingsModel } from "./useFileSettingsModel";

interface PartProps {
  model: FileSettingsModel;
  className?: string;
}

type FileVariant = "classic" | "console" | "fluent" | "cupertino";

interface ActionProps extends PartProps {
  variant?: FileVariant;
}

export function RunProfileBadge({ entry, model, className = "profile-card-badge" }: PartProps & { entry: ProfileEntry | null }) {
  const { t } = useI18n();
  if (!entry || !model.profiles.runProfileAttributes(entry)["data-run-profile"]) return null;
  return <span className={className} {...model.profiles.runProfileAttributes(entry)}>{t("files.runProfileBadge")}</span>;
}

export function DesignateAction({ model, className = "button designate", variant = "classic" }: ActionProps) {
  const { t } = useI18n();
  const { busy } = model.files;
  const { dirtyCount } = model.document;
  const size = variant === "console" ? 14 : variant === "fluent" ? 16 : 17;
  return (
    <button className={className} disabled={!model.document.canDesignate} onClick={() => void model.document.designate()} title={dirtyCount > 0 ? t("profiles.saveBeforeDesignate") : undefined} type="button">
      {variant !== "cupertino" && <><BadgeCheck aria-hidden="true" size={size} strokeWidth={variant === "fluent" ? 1.6 : 2} /> </>}{busy === "designate" ? t("profiles.designating") : t("profiles.designate")}
    </button>
  );
}

export function StartServiceNowAction({ model, className = "text-action", variant = "classic" }: ActionProps) {
  const { t } = useI18n();
  const { busy } = model.files;
  if (!model.document.offerStart) return null;
  const size = variant === "console" ? 12 : variant === "cupertino" ? 13 : 14;
  const strokeWidth = variant === "fluent" ? 1.6 : variant === "cupertino" ? 1.8 : 2;
  return <button className={className} disabled={busy !== null} onClick={() => void model.document.startServiceNow()} type="button"><Play aria-hidden="true" size={size} strokeWidth={strokeWidth} /> {busy === "start" ? t("execution.serviceWorking") : t("files.startServiceNow")}</button>;
}

type SummaryVariant = "path" | "editing" | "title" | "description" | "encoding" | "raw-encoding" | "line-ending" | "unsaved";

export function CurrentFileSummary({ model, className, variant = "path" }: PartProps & { variant?: SummaryVariant }) {
  const { t } = useI18n();
  const { profile } = model.document;
  switch (variant) {
    case "editing": return <>{profile ? t("files.editing") : t("profiles.none")}</>;
    case "title": return <><CurrentFileSummary model={model} variant="editing" />{profile && <> · <CurrentFileSummary model={model} /></>}</>;
    case "description": return profile ? <>{`${model.document.encodingText} · ${t("files.unsaved")} ${model.document.unsavedText}${!profile.canSave ? ` · ${t("files.readOnly")}` : ""}`}</> : null;
    case "encoding": return <>{model.document.encodingText}</>;
    case "raw-encoding": return <>{profile?.encoding ?? "—"}</>;
    case "line-ending": return <>{profile?.lineEnding ?? "—"}</>;
    case "unsaved": return <>{model.document.unsavedText}</>;
    default: return <code className={className} title={profile?.path}>{profile?.displayPath ?? t("profiles.none")}</code>;
  }
}

export function FileMessages({ model, className = "success-message", variant = "classic" }: ActionProps) {
  const { message, error } = model.messages;
  const Tag = variant === "console" ? "span" : "p";
  return <>
    {message && <Tag aria-live={variant === "console" ? undefined : "polite"} className={className} data-operation="file-settings">
      <Check aria-hidden="true" size={variant === "console" ? 14 : 16} /> {message}
      <StartServiceNowAction className={variant === "fluent" ? "text-action fluent-link" : "text-action"} model={model} variant={variant} />
    </Tag>}
    {error && <Tag className="inline-error"><AlertTriangle aria-hidden="true" size={variant === "console" ? 14 : 15} /> {error}</Tag>}
  </>;
}

export function FileNameDialog({ model }: PartProps) {
  if (!model.files.nameDialogOpen) return null;
  return <ProfileNameDialog busy={model.files.documentBusy} initialName={model.files.suggestedProfileName} onCancel={() => model.files.setNameDialogOpen(false)} onSubmit={(name) => void model.files.duplicate(name).then((saved) => {
    if (saved) model.files.setNameDialogOpen(false);
  })} verdict={model.files.profileNameVerdict} />;
}
