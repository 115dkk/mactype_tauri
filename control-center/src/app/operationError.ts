import type { I18nValue, MessageKey } from "../i18n/i18n";

const INTERNAL_OPERATION_FAILURE_PREFIX = "control-center-internal-operation-failed:";

export function operationErrorMessage(
  caught: unknown,
  t: I18nValue["t"],
  internalMessage: MessageKey = "execution.operationFailed",
): string {
  const detail = caught instanceof Error ? caught.message : String(caught);
  return detail.startsWith(INTERNAL_OPERATION_FAILURE_PREFIX) ? t(internalMessage) : detail;
}
